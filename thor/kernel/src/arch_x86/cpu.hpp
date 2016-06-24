
#include <frigg/arch_x86/gdt.hpp>
#include <frigg/arch_x86/idt.hpp>
#include <frigg/arch_x86/tss.hpp>

namespace thor {

// --------------------------------------------------------
// Global runtime functions
// --------------------------------------------------------

struct UniqueKernelStack {
	static constexpr size_t kSize = 0x2000;

	static UniqueKernelStack make();

	friend void swap(UniqueKernelStack &a, UniqueKernelStack &b) {
		frigg::swap(a._pointer, b._pointer);
	}

	UniqueKernelStack()
	: _pointer(nullptr) { }

	UniqueKernelStack(const UniqueKernelStack &other) = delete;

	UniqueKernelStack(UniqueKernelStack &&other)
	: UniqueKernelStack() {
		swap(*this, other);
	}

	UniqueKernelStack &operator= (UniqueKernelStack other) {
		swap(*this, other);
		return *this;
	}

	void *base() {
		return _pointer + kSize;
	}

private:
	explicit UniqueKernelStack(char *pointer)
	: _pointer(pointer) { }

	char *_pointer;
};

struct FaultImagePtr {
	Word *code() { return &_frame()->code; }

	Word *ip() { return &_frame()->rip; }

private:
	// note: this struct is accessed from assembly.
	// do not change the field offsets!
	struct Frame {
		Word rax;
		Word rbx;
		Word rcx;
		Word rdx;
		Word rsi;
		Word rdi;
		Word r8;
		Word r9;
		Word r10;
		Word r11;
		Word r12;
		Word r13;
		Word r14;
		Word r15;
		Word rbp;
		Word code;

		// the following fields are pushed by interrupt
		Word rip;
		Word cs;
		Word rflags;
		Word rsp;
		Word ss;
	};

	Frame *_frame() {
		return reinterpret_cast<Frame *>(_pointer);
	}

	char *_pointer;
};

struct IrqImagePtr {

private:
	char *_pointer;
};

struct SyscallImagePtr {
	Word *number() { return &_frame()->rdi; }
	Word *in0() { return &_frame()->rsi; }
	Word *in1() { return &_frame()->rdx; }
	Word *in2() { return &_frame()->rax; }
	Word *in3() { return &_frame()->r8; }
	Word *in4() { return &_frame()->r9; }
	Word *in5() { return &_frame()->r10; }
	Word *in6() { return &_frame()->r12; }
	Word *in7() { return &_frame()->r13; }
	Word *in8() { return &_frame()->r14; }

	Word *error() { return &_frame()->rdi; }
	Word *out0() { return &_frame()->rsi; }
	Word *out1() { return &_frame()->rdx; }

private:
	// this struct is accessed from assembly.
	// do not randomly change its contents.
	struct Frame {
		Word rdi;
		Word rsi;
		Word rdx;
		Word rax;
		Word r8;
		Word r9;
		Word r10;
		Word r12;
		Word r13;
		Word r14;
		Word rbp;
		Word rsp;
		Word rip;
		Word rflags;
	};

	Frame *_frame() {
		return reinterpret_cast<Frame *>(_pointer);
	}
	
	char *_pointer;
};

struct ExecutorImagePtr {
	static size_t determineSize();

	static ExecutorImagePtr make();

	ExecutorImagePtr()
	: _pointer(nullptr) { }

	// FIXME: remove or refactor the rdi / rflags accessors
	// as they are platform specific and need to be abstracted here
	Word *rdi() { return &_general()->rdi; }
	Word *rflags() { return &_general()->rflags; }

	Word *ip() { return &_general()->rip; }
	Word *sp() { return &_general()->rsp; }
	uint8_t *kernel() { return &_general()->kernel; }

private:
	// note: this struct is accessed from assembly.
	// do not change the field offsets!
	struct General {
		Word rax;			// offset 0x00
		Word rbx;			// offset 0x08
		Word rcx;			// offset 0x10
		Word rdx;			// offset 0x18
		Word rsi;			// offset 0x20
		Word rdi;			// offset 0x28
		Word rbp;			// offset 0x30

		Word r8;			// offset 0x38
		Word r9;			// offset 0x40
		Word r10;			// offset 0x48
		Word r11;			// offset 0x50
		Word r12;			// offset 0x58
		Word r13;			// offset 0x60
		Word r14;			// offset 0x68
		Word r15;			// offset 0x70
		
		Word rsp;			// offset 0x78
		Word rip;			// offset 0x80
		Word rflags;		// offset 0x88
		// 0 = thread saved in user mode
		// 1 = thread saved in kernel mode
		uint8_t kernel;		// offset 0x90
		uint8_t padding[15];
	};
	static_assert(sizeof(General) == 0xA0, "Bad sizeof(General)");

	struct FxState {
		uint16_t fcw; // x87 control word
		uint16_t fsw; // x87 status word
		uint8_t ftw; // x87 tag word
		uint8_t reserved0;
		uint16_t fop;
		uint64_t fpuIp;
		uint64_t fpuDp;
		uint32_t mxcsr;
		uint32_t mxcsrMask;
		uint8_t st0[10];
		uint8_t reserved1[6];
		uint8_t st1[10];
		uint8_t reserved2[6];
		uint8_t st2[10];
		uint8_t reserved3[6];
		uint8_t st3[10];
		uint8_t reserved4[6];
		uint8_t st4[10];
		uint8_t reserved5[6];
		uint8_t st5[10];
		uint8_t reserved6[6];
		uint8_t st6[10];
		uint8_t reserved7[6];
		uint8_t st7[10];
		uint8_t reserved8[6];
		uint8_t xmm0[16];
		uint8_t xmm1[16];
		uint8_t xmm2[16];
		uint8_t xmm3[16];
		uint8_t xmm4[16];
		uint8_t xmm5[16];
		uint8_t xmm6[16];
		uint8_t xmm7[16];
		uint8_t xmm8[16];
		uint8_t xmm9[16];
		uint8_t xmm10[16];
		uint8_t xmm11[16];
		uint8_t xmm12[16];
		uint8_t xmm13[16];
		uint8_t xmm14[16];
		uint8_t xmm15[16];
		uint8_t reserved9[48];
		uint8_t available[48];
	};
	static_assert(sizeof(FxState) == 512, "Bad sizeof(FxState)");

	explicit ExecutorImagePtr(char *pointer)
	: _pointer(pointer) { }

	General *_general() {
		return reinterpret_cast<General *>(_pointer);
	}

	char *_pointer;
};

void saveExecutorFromIrq(IrqImagePtr base);

// copies the current state into the executor and continues normal control flow.
// returns 1 when the state is saved and 0 when it is restored.
extern "C" [[ gnu::returns_twice ]] int forkExecutor();

// restores the current executor from its saved image.
// this is functions does the heavy lifting during task switch.
extern "C" [[ noreturn ]] void restoreExecutor();

size_t getStateSize();

struct ThorRtThreadState {
	ThorRtThreadState();
	~ThorRtThreadState();

	ThorRtThreadState(const ThorRtThreadState &other) = delete;
	
	ThorRtThreadState &operator= (const ThorRtThreadState &other) = delete;

	void activate();
	void deactivate();
	
	ExecutorImagePtr image;
	UniqueKernelStack kernelStack;
	frigg::arch_x86::Tss64 threadTss;
	Word fsBase;
};

struct ThorRtCpuSpecific {
	uint32_t gdt[8 * 2];
	uint32_t idt[256 * 4];
	frigg::arch_x86::Tss64 tssTemplate;
	UniqueKernelStack systemStack;
};

struct CpuContext;

// note: this struct is accessed from assembly.
// do not change the field offsets!
struct ThorRtKernelGs {
	enum {
		kOffCpuContext = 0x00,
		kOffStateSize = 0x08,
		kOffExecutorImage = 0x10,
		kOffSyscallStackPtr = 0x18,
		kOffFlags = 0x20,
		kOffCpuSpecific = 0x28
	};

	enum {
		// there are no flags for now
	};

	ThorRtKernelGs();

	CpuContext *cpuContext;				// offset 0x00
	size_t stateSize;					// offset 0x08
	// TODO: this was syscallState before. tidy up this struct
	ExecutorImagePtr executorImage;					// offset 0x10
	// TODO: move this to the executor state
	void *syscallStackPtr;				// offset 0x18
	uint32_t flags;						// offset 0x20
	uint32_t padding;
	ThorRtCpuSpecific *cpuSpecific;		// offset 0x28
};

CpuContext *getCpuContext();

bool intsAreAllowed();
void allowInts();

// calls the given function on the per-cpu stack
// this allows us to implement a save exit-this-thread function
// that destroys the thread together with its kernel stack
void callOnCpuStack(void (*function) ()) __attribute__ (( noreturn ));

void initializeThisProcessor();

void bootSecondary(uint32_t secondary_apic_id);

// note: this struct is accessed from assembly.
// do not change the field offsets!
struct AdditionalSyscallState {
	Word rax;	// offset 0x00
	Word rbx;	// offset 0x08
	Word rcx;	// offset 0x10
	Word rdx;	// offset 0x18
	Word rdi;	// offset 0x20
	Word rsi;	// offset 0x28
	Word rbp;	// offset 0x30
	Word r8;	// offset 0x38
	Word r9;	// offset 0x40
	Word r10;	// offset 0x48
	Word r11;	// offset 0x50
	Word r12;	// offset 0x58
	Word r13;	// offset 0x60
	Word r14;	// offset 0x68
	Word r15;	// offset 0x70
};

static_assert(sizeof(AdditionalSyscallState) == 0x78, "Bad sizeof(AdditionalSyscallState)");

extern "C" __attribute__ (( noreturn ))
void jumpFromSyscall(AdditionalSyscallState *state);

} // namespace thor

