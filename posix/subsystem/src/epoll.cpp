
#include <string.h>
#include <iostream>

#include <async/doorbell.hpp>
#include <boost/intrusive/list.hpp>
#include <helix/ipc.hpp>
#include "common.hpp"
#include "epoll.hpp"

namespace {

bool logEpoll = false;

struct OpenFile : File {
	// ------------------------------------------------------------------------
	// Internal API.
	// ------------------------------------------------------------------------
private:
	// Lifetime management: There are the following three state bits for each item.
	// Items are deleted once all state bits are zero.
	// Items must only be accessed while a precondition guarantees that
	// at least one state bit is non-zero.
	using State = uint32_t;
	static constexpr State stateActive = 1;
	static constexpr State statePolling = 2;
	static constexpr State statePending = 4;

	struct Item : boost::intrusive::list_base_hook<> {
		Item(smarter::shared_ptr<OpenFile> epoll, Process *process,
				smarter::shared_ptr<File> file, int mask, uint64_t cookie)
		: epoll{epoll}, state{stateActive}, process{process},
				file{std::move(file)}, eventMask{mask}, cookie{cookie} { }

		smarter::shared_ptr<OpenFile> epoll;
		State state;

		// Basic data of this item.
		Process *process;
		smarter::shared_ptr<File> file;
		int eventMask;
		uint64_t cookie;

		async::cancellation_event cancelPoll;
		expected<PollResult> pollFuture;
	};

	static void _awaitPoll(Item *item) {
		assert(item->state & statePolling);
		auto self = item->epoll.get();

		// Release the future to free up memory.
		assert(item->pollFuture.ready());
		auto result_or_error = std::move(item->pollFuture.value());
		item->pollFuture = expected<PollResult>{};

		// Discard non-active and closed items.
		if(!(item->state & stateActive)) {
			item->state &= ~statePolling;
			// TODO: We might have polling + pending items in the future.
			assert(!item->state);
			delete item;
			return;
		}

		auto error = std::get_if<Error>(&result_or_error);
		if(error) {
			assert(*error == Error::fileClosed);
			item->state &= ~statePolling;
			if(!item->state)
				delete item;
			return;
		}

		// Note that items only become pending if there is an edge.
		// This is the correct behavior for edge-triggered items.
		// Level-triggered items stay pending until the event disappears.
		auto result = std::get<PollResult>(result_or_error);
		if(std::get<1>(result) & (item->eventMask | EPOLLERR | EPOLLHUP)) {
			if(logEpoll)
				std::cout << "posix.epoll \e[1;34m" << item->epoll->structName() << "\e[0m"
						<< ": Item \e[1;34m" << item->file->structName()
						<< "\e[0m becomes pending" << std::endl;

			// Note that we stop watching once an item becomes pending.
			// We do this as we have to poll() again anyway before we report the item.
			item->state &= ~statePolling;
			if(!(item->state & statePending)) {
				item->state |= statePending;

				self->_pendingQueue.push_back(*item);
				self->_currentSeq++;
				self->_statusBell.ring();
			}
		}else{
			// Here, we assume that the lambda does not execute on the current stack.
			// TODO: Use some callback queueing mechanism to ensure this.
			if(logEpoll)
				std::cout << "posix.epoll \e[1;34m" << item->epoll->structName() << "\e[0m"
						<< ": Item \e[1;34m" << item->file->structName()
						<< "\e[0m still not pending after poll()."
						<< " Mask is " << item->eventMask << ", while edges are "
						<< std::get<1>(result) << std::endl;
			item->cancelPoll.reset();
			item->pollFuture = item->file->poll(item->process, std::get<0>(result),
					item->cancelPoll);
			item->pollFuture.then([item] {
				_awaitPoll(item);
			});
		}
	}

public:
	~OpenFile() {
		// Nothing to do here.
	}

	void addItem(Process *process, smarter::shared_ptr<File> file, int mask, uint64_t cookie) {
		if(logEpoll)
			std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Adding item \e[1;34m"
					<< file->structName() << "\e[0m. Mask is " << mask << std::endl;
		// TODO: Fix the memory-leak.
		assert(_fileMap.find(file.get()) == _fileMap.end());
		auto item = new Item{smarter::static_pointer_cast<OpenFile>(weakFile().lock()),
				process, std::move(file), mask, cookie};
		item->state |= statePending;

		_fileMap.insert({item->file.get(), item});

		_pendingQueue.push_back(*item);
		_currentSeq++;
		_statusBell.ring();
	}

	void modifyItem(File *file, int mask, uint64_t cookie) {
		if(logEpoll)
			std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Modifying item \e[1;34m"
					<< file->structName() << "\e[0m. New mask is " << mask << std::endl;
		auto it = _fileMap.find(file);
		assert(it != _fileMap.end());
		auto item = it->second;
		assert(item->state & stateActive);

		item->eventMask = mask;
		item->cookie = cookie;
		item->cancelPoll.cancel();

		// Mark the item as pending.
		if(!(item->state & statePending)) {
			item->state |= statePending;

			_pendingQueue.push_back(*item);
			_currentSeq++;
			_statusBell.ring();
		}
	}

	void deleteItem(File *file) {
		if(logEpoll)
			std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Deleting item \e[1;34m"
					<< file->structName() << "\e[0m" << std::endl;
		auto it = _fileMap.find(file);
		assert(it != _fileMap.end());
		auto item = it->second;
		assert(item->state & stateActive);

		item->cancelPoll.cancel();

		_fileMap.erase(it);
		item->state &= ~stateActive;
		if(!item->state)
			delete item;
	}

	async::result<size_t>
	waitForEvents(struct epoll_event *events, size_t max_events,
			async::cancellation_token cancellation) {
		assert(max_events);
		if(logEpoll) {
			std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Entering wait."
					" There are " << _pendingQueue.size() << " pending items; cancellation is "
					<< (cancellation.is_cancellation_requested() ? "active" : "inactive")
					<< std::endl;
		}

		size_t k = 0;
		boost::intrusive::list<Item> repoll_queue;
		while(true) {
			// TODO: Stop waiting in this case.
			assert(isOpen());

			while(!_pendingQueue.empty()) {
				auto item = &_pendingQueue.front();
				_pendingQueue.pop_front();
				assert(item->state & statePending);

				// Discard non-alive items without returning them.
				if(!(item->state & stateActive)) {
					if(logEpoll)
						std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Discarding"
								" inactive item \e[1;34m" << item->file->structName() << "\e[0m"
								<< std::endl;
					item->state &= ~statePending;
					if(!item->state)
						delete item;
					continue;
				}

				if(logEpoll)
					std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Checking item "
							<< "\e[1;34m" << item->file->structName() << "\e[0m" << std::endl;
				auto result_or_error = co_await item->file->checkStatus(item->process);

				// Discard closed items.
				auto error = std::get_if<Error>(&result_or_error);
				if(error) {
					assert(*error == Error::fileClosed);
					if(logEpoll)
						std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Discarding"
								" closed item \e[1;34m" << item->file->structName() << "\e[0m"
								<< std::endl;
					item->state &= ~statePending;
					if(!item->state)
						delete item;
					continue;
				}

				auto result = std::get<PollResult>(result_or_error);
				if(logEpoll)
					std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m:"
							" Item \e[1;34m" << item->file->structName() << "\e[0m"
							" mask is " << item->eventMask << ", while " << std::get<2>(result)
							<< " is active" << std::endl;

				// Abort early (i.e before requeuing) if the item is not pending.
				auto status = std::get<2>(result) & (item->eventMask | EPOLLERR | EPOLLHUP);
				if(!status) {
					item->state &= ~statePending;
					if(!(item->state & statePolling)) {
						item->state |= statePolling;

						// Once an item is not pending anymore, we continue watching it.
						item->cancelPoll.reset();
						item->pollFuture = item->file->poll(item->process, std::get<0>(result),
								item->cancelPoll);
						item->pollFuture.then([item] {
							_awaitPoll(item);
						});
					}
					continue;
				}

				// We have to increment the sequence again as concurrent waiters
				// might have seen an empty _pendingQueue.
				// TODO: Edge-triggered watches should not be requeued here.
				repoll_queue.push_back(*item);

				assert(k < max_events);
				memset(events + k, 0, sizeof(struct epoll_event));
				events[k].events = status;
				events[k].data.u64 = item->cookie;

				k++;
				if(k == max_events)
					break;
			}

			if(k)
				break;

			// Block and re-check if there are pending events.
			if(cancellation.is_cancellation_requested())
				break;

			auto future = _statusBell.async_wait();
			async::result_reference<void> ref = future;
			async::cancellation_callback cancel_callback{cancellation, [&] {
				_statusBell.cancel_async_wait(ref);
			}};
			co_await std::move(future);
		}

		// Before returning, we have to reinsert the level-triggered events that we report.
		if(!repoll_queue.empty()) {
			_pendingQueue.splice(_pendingQueue.end(), repoll_queue);
			_currentSeq++;
			_statusBell.ring();
		}

		if(logEpoll)
			std::cout << "posix.epoll \e[1;34m" << structName() << "\e[0m: Return from wait"
					" with " << k << " items" << std::endl;

		co_return k;
	}

	// ------------------------------------------------------------------------
	// File implementation.
	// ------------------------------------------------------------------------

	void handleClose() override {
		auto it = _fileMap.begin();
		while(it != _fileMap.end()) {
			auto item = it->second;
			assert(item->state & stateActive);

			it = _fileMap.erase(it);
			item->state &= ~stateActive;

			if(item->state & statePolling)
				item->cancelPoll.cancel();

			if(item->state & statePending) {
				auto qit = _pendingQueue.iterator_to(*item);
				_pendingQueue.erase(qit);
				item->state &= ~statePending;
			}

			if(!item->state)
				delete item;
		}

		_statusBell.ring();
		_cancelServe.cancel();
	}

	expected<PollResult> poll(Process *, uint64_t past_seq,
			async::cancellation_token cancellation) override {
		assert(past_seq <= _currentSeq);
		while(_currentSeq == past_seq && !cancellation.is_cancellation_requested()) {
			assert(isOpen()); // TODO: Return a poll error here.
			co_await _statusBell.async_wait(cancellation);
		}
		if(cancellation.is_cancellation_requested())
			std::cout << "\e[33mposix: epoll::poll() cancellation is untested\e[39m" << std::endl;

		co_return PollResult{_currentSeq, EPOLLIN, _pendingQueue.empty() ? 0 : EPOLLIN};
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

public:
	static void serve(smarter::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &File::fileOperations, file->_cancelServe));
	}

	OpenFile()
	: File{StructName::get("epoll"), File::defaultPipeLikeSeek}, _currentSeq{1} { }

private:
	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	// FIXME: This really has to map std::weak_ptrs or std::shared_ptrs.
	std::unordered_map<File *, Item *> _fileMap;

	boost::intrusive::list<Item> _pendingQueue;
	async::doorbell _statusBell;
	uint64_t _currentSeq;
};

} // anonymous namespace

namespace epoll {

smarter::shared_ptr<File, FileHandle> createFile() {
	auto file = smarter::make_shared<OpenFile>();
	file->setupWeakFile(file);
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

void addItem(File *epfile, Process *process, smarter::shared_ptr<File> file,
		int flags, uint64_t cookie) {
	auto epoll = static_cast<OpenFile *>(epfile);
	epoll->addItem(process, std::move(file), flags, cookie);
}

void modifyItem(File *epfile, File *file, int flags, uint64_t cookie) {
	auto epoll = static_cast<OpenFile *>(epfile);
	epoll->modifyItem(file, flags, cookie);
}

void deleteItem(File *epfile, File *file, int flags) {
	assert(!flags);
	auto epoll = static_cast<OpenFile *>(epfile);
	epoll->deleteItem(file);
}

async::result<size_t> wait(File *epfile, struct epoll_event *events,
		size_t max_events, async::cancellation_token cancellation) {
	auto epoll = static_cast<OpenFile *>(epfile);
	return epoll->waitForEvents(events, max_events, cancellation);
}

} // namespace epoll

