#include "memory.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

namespace pen
{
	void* memory_alloc( u32 size_bytes )
	{
		return malloc( size_bytes );
	}
    
    void* memory_realloc( void* mem, u32 size_bytes )
    {
        return realloc( mem, size_bytes );
    }

	void memory_free( void* mem )
	{
		free( mem );
	}

	void memory_zero( void* dest, u32 size_bytes )
	{
		memory_set( dest, 0x00, size_bytes );
	}

	void memory_set( void* dest, u8 val, u32 size_bytes )
	{
		memset( dest, val, size_bytes );
	}

	void memory_cpy( void* dest, const void* src, u32 size_bytes )
	{
		memcpy( dest, src, size_bytes );
	}

	void* memory_alloc_align( u32 size_bytes, u32 alignment )
	{
        void* mem;
        posix_memalign( &mem, size_bytes, alignment );
        
        return mem;
	}

	void memory_free_align( void* mem )
	{
        free( mem );
	}
}

void*	operator new(size_t n) throw(std::bad_alloc)
{
	return pen::memory_alloc( n );
}

void	operator delete(void *p) throw()
{
	pen::memory_free( p );
}