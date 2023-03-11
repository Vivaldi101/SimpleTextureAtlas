//
// stack based allocator
//

struct MemoryStack 
{
    byte *	base;
    size_t	max_size;
    size_t	bytes_used;
    u32 elementCount;
};

#define GetArrayElements(stack, type) ((type *)stack.base)
#define GetAt(stack, type, index) ((type *)_GetAt_(stack, sizeof(type), index))
#define GetLast(stack, type) ((type *)_GetLastElement_(stack, sizeof(type)))
#define PushStruct(stack, type) ((type *)_Push_(stack, sizeof(type)))  

#define PushSize(stack, size, type) ((type *)_Push_(stack, size))  
#define PopStruct(stack, type) ((type *)_Pop_(stack, sizeof(type)))  

#define PushArray(stack, count, type) ((type *)_Push_(stack, (count) * sizeof(type)))
#define PopArray(stack, count, type) ((type *)_Pop_(stack, (count) * sizeof(type)))
#define CheckMemory(cond) do { if (!(cond)) { MessageBoxA(0, "Out of memory in: " ##__FILE__, 0, 0); DebugBreak(); } } while(0)
#define PrintMessageBox(msg) MessageBoxA(0, msg, 0, 0)


static void * 
GetTopMemoryStack(MemoryStack *ms) 
{
    void *ptr = ms->base + ms->bytes_used;
    return ptr;
}

// NOTE: dont use the _Push_ and _Pop_ functions directly, go through the macros
// FIXME: pass alignment
static void *
_Push_(MemoryStack *ms, size_t num_bytes) 
{
    CheckMemory((ms->bytes_used + num_bytes) <= ms->max_size);
    void *result = ms->base + ms->bytes_used;
    ms->bytes_used += num_bytes;
    ms->elementCount++;
    
    return result;
}

static void * 
_Pop_(MemoryStack *ms, size_t num_bytes) 
{
    void *result;
    ms->bytes_used -= num_bytes;
    result = ms->base + ms->bytes_used;
    ms->elementCount--;
    
    CheckMemory(((int)ms->bytes_used) >= 0);
    return result;
}

static MemoryStack 
InitStackMemory(size_t num_bytes) 
{
    MemoryStack result = {};
    void *ptr = VirtualAlloc(0, num_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    
    result.base = (byte *)ptr;
    result.max_size = num_bytes;
    result.bytes_used = 0;
    
    CheckMemory(result.base);
    return result;
}

static size_t
isMemoryStackEmpty(MemoryStack *ms)
{
    return(!(ms->bytes_used > 0));
}

static void
FreeMemoryStack(MemoryStack *ms)
{
    if(ms->base)
    {
        VirtualFree(ms->base, 0, MEM_RELEASE);
        ms->max_size = 0;
        ms->bytes_used = 0;
    }
}

static u32
GetNumMemoryStackElements(MemoryStack *ms, size_t element_size)
{
    u32 result = (u32)(ms->bytes_used / element_size);
    return result;
}

static void *
_GetLastElement_(MemoryStack *ms, size_t element_size)
{
    void *result = nullptr;
    u32 elementCount = ms->elementCount-1;
    result = ms->base + elementCount*element_size;
    
    return result;
}

static void *
_GetAt_(MemoryStack *ms, size_t element_size, u32 index)
{
    void *result = nullptr;
    result = ms->base + index*element_size;
    
    return result;
}

