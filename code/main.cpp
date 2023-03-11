#include <cstdio>
#include <stdint.h>
#include <assert.h>
#include <Windows.h>

// For LRU cache.
#include <unordered_map>


#define KILOBYTES(Value) ((Value)*1024ULL)
#define MEGABYTES(Value) ((KILOBYTES(Value)*1024ULL))
#define GIGABYTES(Value) ((MEGABYTES(Value)*1024ULL))
#define TERABYTES(Value) ((GIGABYTES(Value)*1024ULL))

#define insertAsFirstIntoList(sentinel, element)  \
(element)->prev = (sentinel);       \
(element)->next = (sentinel)->next; \
(sentinel)->next = (element);  \
(element)->next->prev = (element);  \

#define initList(sentinel) \
(sentinel)->next = (sentinel); \
(sentinel)->prev = (sentinel);

#define removeElementFromList(element) \
(element)->prev->next = (element)->next; \
(element)->next->prev = (element)->prev;

#define removeLRUFromList(sentinel) \
(sentinel)->prev->prev->next = (sentinel); \
(sentinel)->prev = (sentinel)->prev->prev; \


typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef unsigned char byte;

typedef u32 b32;

typedef float r32;
typedef double r64;

#include "dynamic_stack.cpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static const char* globalFolderPath;

#define TIMER_RESOLUTION 1
static u64 globalTimeFreq = 0;

static void beginTimer()
{
    timeBeginPeriod(TIMER_RESOLUTION);
}

static void endTimer()
{
    timeEndPeriod(TIMER_RESOLUTION);
}

static void initTimer()
{
    beginTimer();
    LARGE_INTEGER perfCounterFreq;
    QueryPerformanceFrequency(&perfCounterFreq);
    globalTimeFreq = perfCounterFreq.QuadPart;
}

static u64 getMicroseconds()
{
    u64 result;
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    
    result = (s64)(((r64)counter.QuadPart / globalTimeFreq)*1000000);
    
    return result;
}

struct FileHandle
{
    HANDLE win32Handle;
    bool isError;
};

struct FileGroup
{
    u32 fileCount;
    FileHandle handle;
    WIN32_FIND_DATAA data;
};

struct Texture
{
    const char* fileName;
    void* memory;
    u32 bpp;
    u16 x;   // NOTE: in pixel coordinates
    u16 y;
    u16 width;
    u16 height;
};

struct TextureAtlasMetadata
{
    MemoryStack textureArena;
    MemoryStack textureNodeArena;
    MemoryStack fileNameArena;
    u32 textureCount;
    u32 maxSize;
    u32 width;
    u32 height;
    u32 bpp;
};

struct TextureRectangle
{
    u16 left;
    u16 top;
    u16 right;
    u16 bottom;
    union
    {
        struct 
        {
            u16 width;
            u16 height;
        };
        u16 keys[2];
    };
};

enum struct Partition
{
    NONE,
    VERTICAL,
    HORIZONTAL
};

struct TextureNode
{
    TextureNode* left;
    TextureNode* right;
    TextureRectangle block;     
    Partition splitDir;
    bool isUsed;
    bool isDrawn;
};

struct LRUNode
{
    TextureNode* textureNode;
    Texture* texture;
    LRUNode* prev;
    LRUNode* next;
};

struct LRUCache
{
    LRUNode* sentinel;
    std::unordered_map<Texture*, LRUNode*> hashLookup;
    u16 atlasWidth;
    u16 atlasHeight;
    u32 nodeCount;
    MemoryStack arena;
};

static LRUCache makeLRUList(u32 listSize)
{
    LRUCache result = {};
    
    // +1 for the sentinel.
    result.arena = InitStackMemory(listSize + sizeof(LRUNode));
    result.sentinel = PushStruct(&result.arena, LRUNode);
    initList(result.sentinel);
    
    return result;
}

static void clearLRUCache(LRUCache* cache)
{
    printf("Clearing LRU cache\n");
    u32 elementCount = cache->arena.elementCount;
    for(u32 i = 0; i < elementCount; i++)
    {
        PopStruct(&cache->arena, LRUNode);
    }
    cache->nodeCount = 0;
    cache->atlasWidth = 0;
    cache->atlasHeight = 0;
    cache->hashLookup.clear();
    initList(cache->sentinel);
}

static void insertIntoLRUCache(TextureNode* textureNode, Texture* texture, LRUCache* cache, u16 currentAtlasWidth, u16 currentAtlasHeight)
{
    // The node exists in the lookup table.
    if(cache->hashLookup.find(texture) != cache->hashLookup.end())
    {
//        printf("Moving an existing node to the head of cache[%ux%u]\n", texture->width, texture->height);
        LRUNode* cachedNode = cache->hashLookup[texture];
        insertAsFirstIntoList(cache->sentinel, cachedNode);
    }
    else
    {
//        printf("Inserting a new node as first into cache[%ux%u]\n", texture->width, texture->height);
        LRUNode* cachedNode = PushStruct(&cache->arena, LRUNode);
        cachedNode->textureNode = textureNode;
        cachedNode->textureNode->isUsed = true;
        cachedNode->texture = texture;
        insertAsFirstIntoList(cache->sentinel, cachedNode);
        cache->nodeCount++;
        cache->atlasWidth = currentAtlasWidth;
        cache->atlasHeight = currentAtlasHeight;
        
        cache->hashLookup[texture] = cachedNode;
    }
//    printf("Number of nodes in the cache: %u\n", cache->nodeCount);
}

static LRUNode* removeLRUFromCache(LRUCache* cache, std::vector<TextureNode*>* nodePath)
{
    LRUNode* result = nullptr;
    
    // Not an empty cache.
    if(cache->nodeCount)
    {
        LRUNode* lruNode = cache->sentinel->prev;
        // The LRU does exist in the lookup table.
        if(cache->hashLookup.find(lruNode->texture) != cache->hashLookup.end())
        {
//            printf("Removing LRU node from the cache[%ux%u]\n", lruNode->texture->width, lruNode->texture->height);
            cache->hashLookup.erase(lruNode->texture);
            
            lruNode->textureNode->isUsed = false;
            lruNode->textureNode->splitDir = Partition::NONE;
            // TODO: Readjust the subtree path from leaf to root of which the removed lru node is.
            while(!nodePath->empty())
            {
                TextureNode* node = nodePath->front();
//                printf("Node[%ux%u]\n", node->block.width, node->block.height);
                nodePath->pop_back();
            }
            
            result = lruNode;
            removeLRUFromList(cache->sentinel);
            
            cache->nodeCount--;
        }
//        printf("Number of nodes left in the cache: %u\n", cache->nodeCount);
    }
    
    return result;
}

static void removeNodeFromCache(LRUCache* cache, LRUNode* node)
{
    if(node && node->texture)
    {
        if(cache->hashLookup.find(node->texture) == cache->hashLookup.end())
        {
            return;
        }
        cache->hashLookup.erase(node->texture);
        node->textureNode->isUsed = false;
        node->textureNode->left = nullptr;
        node->textureNode->right = nullptr;
        node->textureNode->splitDir = Partition::NONE;
        
        removeElementFromList(node);
        
        cache->nodeCount--;
    }
}

static void contractLRUCache(LRUCache* cache, u16 maxAtlasWidth, u16 maxAtlasHeight)
{
    // Find the LRU elements which cross the max boundaries.
    for(LRUNode* node = cache->sentinel->next; node != cache->sentinel; node = node->next)
    {
        if(node->texture->x >= maxAtlasWidth || node->texture->y >= maxAtlasHeight)
        {
            if(node->texture->x >= maxAtlasWidth)
            {
                cache->atlasWidth -= node->texture->width;
            }
            if(node->texture->y >= maxAtlasHeight)
            {
                cache->atlasHeight -= node->texture->height;
            }
            removeNodeFromCache(cache, node);
            break;
        }
    }
}

static void reportError(const char* msg)
{
    MessageBoxA(0, msg, 0, 0); 
    DebugBreak();
}

static void copyBytes(char* dest, const char* source)
{
    while(*dest++ = *source++) ;
}

static void setPathToWorkingDir(char* path)
{
    copyBytes(path, globalFolderPath);
}

static void appendToPath(char* string, const char* suffix)
{
    char* p = string;
    while(*p++) ;
    p--;
    if(p[-1] != '\\')
    {
        *p++ = '\\';
    }
    copyBytes(p, suffix);
}

static void writeTextureAtlasMetadata(TextureAtlasMetadata* atlasMetadata, LRUCache* cache, char* atlasMetadataName)
{
    char atlasMetadataPath[MAX_PATH];
    setPathToWorkingDir(atlasMetadataPath);
    appendToPath(atlasMetadataPath, atlasMetadataName);
    FILE* atlasMetadataFile = fopen(atlasMetadataPath, "w");
    
    if(atlasMetadataFile)
    {
        fprintf(atlasMetadataFile, "Atlas meta data\n");
        r32 atlasWidth = (r32)atlasMetadata->width;
        r32 atlasHeight = (r32)atlasMetadata->height;
        for(LRUNode* node = cache->sentinel->next; node != cache->sentinel; node = node->next)
        {
            const Texture* texture = node->texture;
            const char* name = texture->fileName;
            u32 x = texture->x;
            u32 y = texture->y;
            u32 width = texture->width;
            u32 height = texture->height;
            r32 u = (r32)x / atlasWidth;
            r32 v = (r32)y / atlasHeight;
            fprintf(atlasMetadataFile, "%s, ", name);
            fprintf(atlasMetadataFile, "%u, ", x);
            fprintf(atlasMetadataFile, "%u, ", y);
            fprintf(atlasMetadataFile, "%f, ", u);
            fprintf(atlasMetadataFile, "%f, ", v);
            fprintf(atlasMetadataFile, "%u, ", width);
            fprintf(atlasMetadataFile, "%u", height);
            fprintf(atlasMetadataFile, "\n");
        }
        fclose(atlasMetadataFile);
    }
    else
    {
        reportError("Error: Unable to write atlas meta data file");
    }
}

static FileHandle* openNextFile(FileGroup* files) 
{
    FileHandle* result = nullptr;
    if(files->handle.win32Handle != INVALID_HANDLE_VALUE)
    {
        result = (FileHandle *)VirtualAlloc(0, sizeof(FileHandle), MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
        if(result)
        {
            result->win32Handle = CreateFileA(files->data.cFileName, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
            result->isError = (result->win32Handle == INVALID_HANDLE_VALUE);
        }
        else
        {
            reportError("Error: Could not allocate memory for file");
        }
        if(!FindNextFileA(files->handle.win32Handle, &files->data))
        {
            FindClose(files->handle.win32Handle);
            files->handle.win32Handle = INVALID_HANDLE_VALUE;
        }
    }
    
    return result;
}

static FileGroup* createFileGroup(const char* path)
{
    FileGroup* result = (FileGroup *)VirtualAlloc(0, sizeof(FileGroup), MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    if(result)
    {
        WIN32_FIND_DATAA fd = {};
        HANDLE h = FindFirstFileA(path, &fd);
        while(h != INVALID_HANDLE_VALUE)
        {
            result->fileCount++;
            if(!FindNextFileA(h, &fd))
            {
                break;
            }
        }
        if(h != INVALID_HANDLE_VALUE)
        {
            FindClose(h);
            result->handle.win32Handle = FindFirstFileA(path, &result->data);
        }
        else
        {
            reportError("Error: Could not find .png file(s) in the specified directory");
        }
        
    }
    else
    {
        reportError("Error: Could not allocate memory for files");
    }
    
    return result;
}

static void freeFileGroup(FileGroup* files)
{
    if(files)
    {
        VirtualFree(files, 0, MEM_RELEASE);
    }
}

static void closeFileGroup(FileGroup* files)
{
    if(files)
    {
        FindClose(files->handle.win32Handle);
    }
}

static void
destroyFileGroup(FileGroup* files)
{
    closeFileGroup(files);
    freeFileGroup(files);
}

static s32 compareHeight(const void* p1, const void* p2)
{
    s32 result = 0;
    
    result = ((s32)((Texture *)p2)->height - (s32)((Texture *)p1)->height);
    return result;
}

static s32 compareWidth(const void* p1, const void* p2)
{
    s32 result = 0;
    
    result = ((s32)((Texture *)p2)->width - (s32)((Texture *)p1)->width);
    return result;
}

static void sortTexturesByHeight(TextureAtlasMetadata* atlasMetadataPath)
{
    qsort(GetArrayElements(atlasMetadataPath->textureArena, Texture), atlasMetadataPath->textureArena.elementCount, sizeof(Texture), compareHeight);
}

enum struct Side
{
    INVALID,
    VERTICAL,
    HORIZONTAL
};

static Side getLongerSide(Texture* textures, u32 textureCount)
{
    Side result = Side::INVALID;
    u32 maxWidth = 0;
    u32 maxHeight = 0;
    for(u32 i = 0; i < textureCount; i++)
    {
        u32 currentWidth = textures[i].width;
        u32 currentHeight = textures[i].height;
        if(currentWidth > maxWidth)
        {
            maxWidth = currentWidth;
        }
        if(currentHeight > maxHeight)
        {
            maxHeight = currentHeight;
        }
        
        if(maxWidth > maxHeight)
        {
            result = Side::HORIZONTAL;
        }
        else
        {
            result = Side::VERTICAL;
        }
    }
    
    return result;
}

static void sortTextures(TextureAtlasMetadata* atlasMetadataPath)
{
    Side longerSide = getLongerSide(GetArrayElements(atlasMetadataPath->textureArena, Texture), atlasMetadataPath->textureArena.elementCount);
    if(longerSide == Side::HORIZONTAL)
    {
        qsort(GetArrayElements(atlasMetadataPath->textureArena, Texture), atlasMetadataPath->textureArena.elementCount, sizeof(Texture), compareWidth);
    }
    else if(longerSide == Side::VERTICAL)
    {
        qsort(GetArrayElements(atlasMetadataPath->textureArena, Texture), atlasMetadataPath->textureArena.elementCount, sizeof(Texture), compareHeight);
    }
    else
    {
        assert(0);
    }
}

static void sortTexturesByWidth(TextureAtlasMetadata* atlasMetadataPath)
{
    qsort(GetArrayElements(atlasMetadataPath->textureArena, Texture), atlasMetadataPath->textureArena.elementCount, sizeof(Texture), compareWidth);
}

static bool isLeaf(TextureNode* node)
{
    bool result = (node->left || node->right);
    
    return result == 0;
}

static bool isBlockExactFit(TextureNode* node, u16 textureWidth, u16 textureHeight)
{
    bool result = (node->block.width == textureWidth) && (node->block.height == textureHeight);
    
    return result;
}

static bool isBlockFit(TextureNode* node, u16 textureWidth, u16 textureHeight)
{
    bool result = (node->block.width >= textureWidth) && (node->block.height >= textureHeight);
    
    return result;
}

static bool isBlockWidthExactFit(TextureNode* node, u16 textureWidth)
{
    bool result = (node->block.width == textureWidth);
    
    return result;
}

static bool isBlockHeightExactFit(TextureNode* node, u16 textureHeight)
{
    bool result = (node->block.height == textureHeight);
    
    return result;
}

static bool isBlockPartiallyExactFit(TextureNode* node, u16 textureWidth, u16 textureHeight)
{
    bool result = (node->block.width == textureWidth) || (node->block.height == textureHeight);
    
    return result;
}

static bool isRotatedBlockFit(TextureNode* node, u16 textureWidth, u16 textureHeight)
{
    bool result = (node->block.width >= textureHeight) && (node->block.height >= textureWidth);
    
    return result;
}

static bool isRotatedBlockExactFit(TextureNode* node, u16 textureWidth, u16 textureHeight)
{
    bool result = (node->block.width == textureHeight) && (node->block.height == textureWidth);
    
    return result;
}

static void splitHorizontallyNew(TextureNode* node, MemoryStack* textureNodeArena, u16 textureWidth, u16 textureHeight)
{
    TextureNode* newLeft = PushStruct(textureNodeArena, TextureNode);
    TextureNode* newRight = PushStruct(textureNodeArena, TextureNode);
    
    node->splitDir = Partition::HORIZONTAL;
    node->left = newLeft;
    node->right = newRight;
    
    newLeft->block.left = node->block.left;
    newLeft->block.top = node->block.top;
    newLeft->block.right = node->block.left + textureWidth - 1;
    newLeft->block.bottom = node->block.bottom;
    
    newRight->block.left = newLeft->block.right + 1;
    newRight->block.top = node->block.top;
    newRight->block.right = node->block.right;
    newRight->block.bottom = node->block.bottom;
    
    newLeft->block.width = (newLeft->block.right - newLeft->block.left) + 1;
    newLeft->block.height = node->block.height;
    
    newRight->block.width = (newRight->block.right - newRight->block.left) + 1;
    newRight->block.height = node->block.height;
    
    assert(newRight->block.width > 0);
    assert(newRight->block.height > 0);
    assert(newLeft->block.width > 0);
    assert(newLeft->block.height > 0);
}

static void splitVerticallyNew(TextureNode* node, MemoryStack* textureNodeArena, u16 textureWidth, u16 textureHeight)
{
    
    TextureNode* newLeft = PushStruct(textureNodeArena, TextureNode);
    TextureNode* newRight = PushStruct(textureNodeArena, TextureNode);
    
    node->splitDir = Partition::VERTICAL;
    node->left = newLeft;
    node->right = newRight;
    
    newLeft->block.left = node->block.left;
    newLeft->block.top = node->block.top;
    newLeft->block.right = node->block.right;
    newLeft->block.bottom = node->block.top + textureHeight - 1;
    
    newRight->block.left = node->block.left;
    newRight->block.top = newLeft->block.bottom + 1;
    newRight->block.right = node->block.right;
    newRight->block.bottom = node->block.bottom;
    
    newLeft->block.width = node->block.width;
    newLeft->block.height = (newLeft->block.bottom - newLeft->block.top) + 1;
    
    newRight->block.width = node->block.width;
    newRight->block.height = (newRight->block.bottom - newRight->block.top) + 1;
    
    assert(newRight->block.width > 0);
    assert(newRight->block.height > 0);
    assert(newLeft->block.width > 0);
    assert(newLeft->block.height > 0);
}

static TextureNode* findFirstFreeBlock(TextureNode* node, MemoryStack* textureNodeArena, Texture* texture)
{
    TextureNode* result = nullptr;
    if(isBlockFit(node, texture->width, texture->height))
    {
        if(isBlockExactFit(node, texture->width, texture->height))
        {
            result = node;
            result->isUsed = true;
            result->splitDir = Partition::NONE;
        }
        else if(isBlockPartiallyExactFit(node, texture->width, texture->height))
        {
            if(isBlockWidthExactFit(node, texture->width))
            {
                splitVerticallyNew(node, textureNodeArena, texture->width, texture->height);
            }
            else if(isBlockHeightExactFit(node, texture->height))
            {
                splitHorizontallyNew(node, textureNodeArena, texture->width, texture->height);
            }
            result = node->left;
            result->isUsed = true;
            result->splitDir = Partition::NONE;
        }
        // Handle empty spaces from expansions.
        else
        {
            if(texture->height > texture->width)
            {
                splitHorizontallyNew(node, textureNodeArena, texture->width, texture->height);
                splitVerticallyNew(node->left, textureNodeArena, texture->width, texture->height);
            }
            else
            {
                splitVerticallyNew(node, textureNodeArena, texture->width, texture->height);
                splitHorizontallyNew(node->left, textureNodeArena, texture->width, texture->height);
            }
            
            result = node->left->left;
            result->isUsed = true;
            result->splitDir = Partition::NONE;
        }
    }
    
    return result;
}

static TextureNode* findFirstFreeRotatedBlock(TextureNode* node, MemoryStack* textureNodeArena, Texture* texture)
{
    TextureNode* result = nullptr;
    if(isRotatedBlockFit(node, texture->width, texture->height))
    {
        if(isRotatedBlockExactFit(node, texture->width, texture->height))
        {
            result = node;
            result->isUsed = true;
        }
        else
        {
            if(texture->width > texture->height)
            {
                //assert(0);
                splitHorizontallyNew(node, textureNodeArena, texture->height, texture->width);
                splitVerticallyNew(node->left, textureNodeArena, texture->height, texture->width);
            }
            //else
            {
                //splitHorizontally(node, textureNodeArena, texture->width, texture->height);
                //splitVertically(node->left, textureNodeArena, texture->width, texture->height);
            }
        }
    }
    
    return result;
}

static void renderBlockIntoTextureAtlas(TextureNode* node, Texture* textureAtlas)
{
    u32 atlasPitch = textureAtlas->width*textureAtlas->bpp;
    u32 atlasBpp = textureAtlas->bpp;
    u32 blockWidth = node->block.width;
    u32 blockHeight = node->block.height;
    
//    printf("Rendering a block(%d,%d, %dx%d)\n", node->block.left, node->block.top, blockWidth, blockHeight);
    
    byte* atlasDestMemory = (byte *)textureAtlas->memory + (node->block.top * atlasPitch) + (node->block.left * atlasBpp);
    u32* atlasDestPixel = (u32*)atlasDestMemory;
    for(u32 i = 0; i < blockHeight; i++)
    {
        for(u32 j = 0; j < blockWidth; j++)
        {
            *atlasDestPixel++ = 0xffff00ff;
        }
        atlasDestMemory += atlasPitch;
    }
}

static TextureNode* traverseTextureNodes(TextureNode* node, MemoryStack* textureNodeArena, Texture* textureAtlas, Texture* texture, std::vector<TextureNode*>* nodePath)
{
    TextureNode* result = nullptr;
    while(node)
    {
        if(!node->isUsed && !node->isDrawn)
        {
            renderBlockIntoTextureAtlas(node, textureAtlas);
            node->isDrawn = true;
        }
        if(isLeaf(node) && !node->isUsed)
        {
            // Visit the actual node.
            return findFirstFreeBlock(node, textureNodeArena, texture);
        }
        else
        {
            // Invariants.
            if(node->splitDir == Partition::VERTICAL)
            {
                assert(node->block.height == (node->left->block.height + node->right->block.height));
            }
            else if(node->splitDir == Partition::HORIZONTAL)
            {
                assert(node->block.width == (node->left->block.width  + node->right->block.width));
            }
            // Pre-check if this texture fits into the split hunks before descending further.
            if(node->splitDir == Partition::VERTICAL)
            {
                if((texture->height <= node->left->block.height))
                {
                    nodePath->push_back(node);
                    result = traverseTextureNodes(node->left, textureNodeArena, textureAtlas, texture, nodePath);
                }
            }
            else if(node->splitDir == Partition::HORIZONTAL)
            {
                if((texture->width <= node->left->block.width))
                {
                    nodePath->push_back(node);
                    result = traverseTextureNodes(node->left, textureNodeArena, textureAtlas, texture, nodePath);
                }
            }
            // Take the first free block.
            if(result)
            {
                break;
            }
            // No partition on this node.
            node = node->right;
        }
    }
    
    return result;
}

static TextureNode* expandRootVertically(TextureNode* root, MemoryStack* textureNodeArena, u16 height)
{
    TextureNode* result = PushStruct(textureNodeArena, TextureNode);
    TextureNode* right = PushStruct(textureNodeArena, TextureNode);
    u16 verticalExpansion = root->block.height + height;
    
    result->block = root->block;
    
    right->left = nullptr;
    right->right = nullptr;
    right->block.left = root->block.left;
    right->block.top = root->block.height;
    right->block.right = root->block.right;
    right->block.bottom = right->block.top + height - 1;
    right->block.width = root->block.width;
    right->block.height = height;
    right->splitDir = Partition::NONE;
    right->isUsed = false;
    
    result->block.height = verticalExpansion;
    result->block.bottom = result->block.height - 1;
    result->left = root;
    result->right = right;
    
    result->splitDir = Partition::VERTICAL;
    
    return result;
}

static TextureNode* expandRootHorizontally(TextureNode* root, MemoryStack* textureNodeArena, u16 width)
{
    TextureNode* result = PushStruct(textureNodeArena, TextureNode);
    TextureNode* right = PushStruct(textureNodeArena, TextureNode);
    u16 horizontalExpansion = root->block.width + width;
    
    result->block = root->block;
    
    right->left = nullptr;
    right->right = nullptr;
    right->block.left = root->block.width;
    right->block.top = root->block.top;
    right->block.right = right->block.left + width - 1;
    right->block.bottom = root->block.bottom;
    right->block.width = width;
    right->block.height = root->block.height;
    right->splitDir = Partition::NONE;
    right->isUsed = false;
    
    result->block.width = horizontalExpansion;
    result->block.right = result->block.width - 1;
    result->left = root;
    result->right = right;
    
    result->splitDir = Partition::HORIZONTAL;
    return result;
}

static void packTexturesIntoAtlas(Texture* textures, MemoryStack* textureNodeArena, u32 textureCount, LRUCache* cache, Texture* textureAtlas)
{
    TextureNode* root = PushStruct(textureNodeArena, TextureNode);
    const u16 maxAtlasWidth = textureAtlas->width;
    const u16 maxAtlasHeight = textureAtlas->height;
    u16 startingWidth = textures[0].width;
    u16 startingHeight = textures[0].height;
    root->left = nullptr;
    root->right = nullptr;
    root->block.left = 0;
    root->block.top = 0;
    root->block.width = startingWidth;
    root->block.height = startingHeight;
    root->block.right = root->block.left + root->block.width - 1;
    root->block.bottom = root->block.top + root->block.height - 1;
    root->isUsed = false;
    
    
    for(u32 textureIndex = 0; textureIndex < textureCount;)
    {
        std::vector<TextureNode*> nodePath;
        Texture* texture = &textures[textureIndex];
        TextureNode* node = traverseTextureNodes(root, textureNodeArena, textureAtlas, texture, &nodePath);
        
        // Found a first free block in the texture atlas to fit the node.
        if(node)
        {
            textureIndex++;
            texture->x = node->block.left;
            texture->y = node->block.top;
            insertIntoLRUCache(node, texture, cache, root->block.width, root->block.height);
        }
        else
        {
            u16 verticalExpansion = root->block.height + texture->height;
            u16 horizontalExpansion = root->block.width + texture->width;
            if((verticalExpansion < horizontalExpansion) && (verticalExpansion <= maxAtlasWidth))
            {
                root = expandRootVertically(root, textureNodeArena, texture->height);
            }
            else if((verticalExpansion >= horizontalExpansion) && (horizontalExpansion <= maxAtlasHeight))
            {
                root = expandRootHorizontally(root, textureNodeArena, texture->width);
            }
            else
            {
                // Cannot expand anymore.
                removeLRUFromCache(cache, &nodePath);
            }
        }
    }
    
    textureAtlas->width = min(root->block.width, maxAtlasWidth);
    textureAtlas->height = min(root->block.height, maxAtlasHeight);
    cache->atlasWidth = textureAtlas->width;
    cache->atlasHeight = textureAtlas->height;
}

static void buildTextureAtlas(Texture* atlas, LRUCache* cache)
{
    u32 atlasPitch = atlas->width*atlas->bpp;
    u32 bpp = atlas->bpp;
    
    for(LRUNode* node = cache->sentinel->next; node != cache->sentinel; node = node->next)
    {
        const Texture* texture = node->texture;
        u32 texture_x = texture->x;
        u32 texture_y = texture->y;
        u32 width = texture->width;
        u32 height = texture->height;
        byte* dest = (byte *)atlas->memory + texture_y*atlasPitch + texture_x*bpp;
        
        {
            u32 texturePitch = width*bpp;
            byte* source = (byte *)texture->memory;
            for(u32 j = 0; j < height; j++)
            {
                memcpy(dest, source, width*bpp);
                dest += atlasPitch;
                source += texturePitch;
            }
        }
    }
}

static Texture generateTextureAtlas(TextureAtlasMetadata* atlasMetadata, LRUCache* cache)
{
    Texture result = {};
    result.x = 0;
    result.y = 0;
    result.width = (u16)atlasMetadata->width;
    result.height = (u16)atlasMetadata->height;
    result.bpp = atlasMetadata->bpp;
    
    sortTextures(atlasMetadata);
    result.memory = PushSize(&atlasMetadata->textureArena, result.width * result.height * result.bpp, byte);
    memset(result.memory, 0, result.bpp * result.width * result.height);
    atlasMetadata->textureArena.elementCount--;
    
    // Figure out the textures xy coordinates in the texture atlas.
    packTexturesIntoAtlas(GetArrayElements(atlasMetadata->textureArena, Texture), &atlasMetadata->textureNodeArena,  atlasMetadata->textureArena.elementCount, cache, &result);
    
    cache->atlasWidth = result.width;
    cache->atlasHeight = result.height;
    
    // Actually build the atlas itself from the textures.
    buildTextureAtlas(&result, cache);
    
    return result;
}

static void writeTextureAtlas(Texture* atlas, u32 textureCount, const char* fileName)
{
    char folderPath[MAX_PATH];
    setPathToWorkingDir(folderPath);
    appendToPath(folderPath, fileName);
    int success = stbi_write_png(folderPath, atlas->width, atlas->height, atlas->bpp, atlas->memory, 0);
    if(success)
    {
        printf("Success writing texture atlas[%dx%d = %zu] of %d textures\n", atlas->width, atlas->height, (size_t)atlas->width*atlas->height, textureCount);
    }
    else
    {
        reportError("Error: Could not write texture atlas to disk");
    }
}

static void loadFiles(FileGroup* files, MemoryStack* textureArena, MemoryStack* fileNameArena, u32 textureCount, u32* textureAtlasBpp)
{
    char folderPath[MAX_PATH];
    setPathToWorkingDir(folderPath);
    
    for(u32 i = 0; i < textureCount; i++)
    {
        appendToPath(folderPath, files->data.cFileName);
        openNextFile(files);
        
        char* fileName = PushArray(fileNameArena, MAX_PATH, char);
        copyBytes(fileName, folderPath);
        setPathToWorkingDir(folderPath);
        
        s32 width;
        s32 height;
        s32 bpp;
        byte* memory = stbi_load(fileName, &width, &height, &bpp, 0);
        
        if(memory)
        {
            Texture* tex = PushStruct(textureArena, Texture);
            tex->width = (u16)width;
            tex->height = (u16)height;
            tex->bpp = bpp;
            tex->memory = memory;
            tex->fileName = fileName;
            
            if(*textureAtlasBpp == 0)
            {
                *textureAtlasBpp = bpp;
            }
            else
            {
                if(*textureAtlasBpp != (u32)bpp)
                {
                    reportError("Error: Textures in given folder have different number of bytes per pixel!");
                }
            }
        }
        else
        {
            reportError("Error: Could not load .png file");
        }
    }
}

static void destroyTextureAtlasMetadata(TextureAtlasMetadata *atlasMetadata)
{
    Texture* textures = GetArrayElements(atlasMetadata->textureArena, Texture);
    u32 textureCount = atlasMetadata->textureArena.elementCount-1;
    
    for(u32 i = 0; i < textureCount; i++)
    {
        Texture* texture = &textures[i];
        stbi_image_free(texture->memory);
    }
    FreeMemoryStack(&atlasMetadata->textureArena);
    FreeMemoryStack(&atlasMetadata->textureNodeArena);
    FreeMemoryStack(&atlasMetadata->fileNameArena);
}

static TextureAtlasMetadata generateTextureAtlasMetadata(u32 width, u32 height, u32 bpp)
{
    TextureAtlasMetadata result = {};
    
    char workingFolderPath[MAX_PATH];
    char wildcardFolderPath[MAX_PATH];
    setPathToWorkingDir(workingFolderPath);
    setPathToWorkingDir(wildcardFolderPath);
    appendToPath(wildcardFolderPath, "*.png");
    
    FileGroup* files = createFileGroup(wildcardFolderPath);
    const u32 textureCount = files->fileCount;
    const u32 textureAtlasSize = width * height * bpp;
    
    MemoryStack textureArena = InitStackMemory(textureCount * sizeof(Texture) + textureAtlasSize);
    
    MemoryStack textureNodeArena = InitStackMemory((1 + textureCount * 4) * sizeof(TextureNode));
    
    MemoryStack fileNameArena = InitStackMemory(textureCount*MAX_PATH);
    
    loadFiles(files, &textureArena, &fileNameArena, textureCount, &result.bpp);
    result.textureArena = textureArena;
    result.textureNodeArena = textureNodeArena;
    result.fileNameArena = fileNameArena;
    result.maxSize = textureAtlasSize;
    result.textureCount = textureCount;
    
    result.width = width;
    result.height = height;
    result.bpp = bpp;
    
    destroyFileGroup(files);
    
    return result;
}

int main(int argc, const char **argv)
{
    const char* programName = argv[0];
    if(argc == 2)
    {
        beginTimer();
        globalFolderPath = argv[1];
        if(strcmp(globalFolderPath, "help") == 0)
        {
            fprintf(stdout, "This program packs .png files into a texture atlas to the specified folder path provided by the user from the command line\n");
            return 0;
        }
        
        printf("Start of program!\n");
        TextureAtlasMetadata atlasMetadata = generateTextureAtlasMetadata(64, 64, 4);
        
        LRUCache cache = makeLRUList(atlasMetadata.textureCount * sizeof(LRUNode));
        printf("Start generating texture atlas...\n");
        Texture textureAtlas = generateTextureAtlas(&atlasMetadata, &cache);
        printf("Texture atlas generated\n");
        
        writeTextureAtlasMetadata(&atlasMetadata, &cache, "atlasMetadata.txt");
        
        writeTextureAtlas(&textureAtlas, cache.nodeCount, "atlas.png");
        destroyTextureAtlasMetadata(&atlasMetadata);
        endTimer();
    }
    else
    {
        fprintf(stderr, "Invalid usage of: %s\n", programName);
        fprintf(stderr, "Valid usage: %s 'path to image folder'\n", programName);
    }
    
    return 0;
}
