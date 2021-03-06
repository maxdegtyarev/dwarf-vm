#include <malloc.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#include <common.h>
#include <function.h>
#include <loader.h>
#include <const_pool.h>
#include <util.h>

#define FILE_MAGIC_FST 0xBA
#define FILE_MAGIC_SND 0xBA

#define READVAR( addr ) \
    do { if (! fread( (addr), sizeof(*(addr)), 1, f) ) return LOAD_IO_ERROR; } while (0)
#define READ( addr, size ) \
    do { if (! fread( (addr), size, 1, f) ) return LOAD_IO_ERROR; } while (0)

#define READ_OR( addr, size, finally ) \
    do { if (! fread( (addr), size, 1, f) ){ finally; return LOAD_IO_ERROR; }  } while (0)

/** @addtogroup loader Loader
 *
 * Provides functions to load compiled bytecode from .dwc files */

enum vm_load_result file_check_signature( FILE* f ) {
    uint8_t sig[2];
    
    READ( sig, 2 );

    if ( sig[0] == FILE_MAGIC_FST && sig[1] == FILE_MAGIC_SND )
        return LOAD_OK;
    return LOAD_INVALID_FORMAT; 
}

enum vm_load_result file_check_version( FILE* f ) {
    uint64_t version;
    READ( &version, 8 );
    if ( FILE_FORMAT_VERSION != version ) return LOAD_INCOMPATIBLE_VERSION; 
    return LOAD_OK;
}


static 
struct vm_const_pool
init_const_pool_str_addrs( char const* pool, size_t sz ) {
    const size_t count  = count_strings( pool, sz );

    struct vm_const_pool result = {
       .pool = pool,
       .sz = sz, 
       .by_id = divide_on_strings( pool, sz, count ), 
       .count = count
    };

    return result; 
}

enum vm_load_result
file_read_consts( FILE* f, struct vm_const_pool* pool ) {
    uint64_t sz;
    READVAR( &sz );
    pool-> sz = sz;
    char* contents = malloc( sz );
    READ_OR( contents, sz, { free( contents ); }  );

    *pool = init_const_pool_str_addrs( contents, sz );
    return LOAD_OK;
}


struct 
__attribute__((packed))
    vm_fun_serialized {
    uint64_t name;
    uint64_t locals_count;
    uint64_t flags;
    uint64_t arg_count;
    uint8_t  arg_types[SIGNATURE_MAX_ARGS]; 
    uint64_t bytecode_sz;
};


static
struct vm_fun deserialize_fun( 
        struct vm_fun_serialized const* serialized 
        ) {

    struct vm_fun result = {
        .meta = {
            .id = 0, 
            .sig = {
                .name = serialized-> name,
                .arg_count = serialized-> arg_count
            },
            .is_native = 0,
            .native_addr = NULL,
            .topmost_present = NULL,
            .locals_count = serialized->locals_count
        },
            .code_sz = serialized-> bytecode_sz,
            .code = NULL
    };

    for ( uint64_t i = 0; i < serialized-> arg_count; i++ )
            result.meta.sig.args[i] = serialized-> arg_types[i];
    return result;
}

enum vm_load_result
file_read_fun( FILE* f, struct vm_fun* fun ) {
    assert( fun );

    struct vm_fun_serialized buf = {0};
    READVAR( &buf );
    *fun = deserialize_fun( &buf ); 
    fun-> code = malloc( fun-> code_sz );

    READ_OR( fun->code, fun-> code_sz, 
            { free( fun-> code ); } ); 
    return LOAD_OK;
}

static void free_funs_code( struct vm_funs* funs, size_t sz ) {
    assert( funs );

    for(size_t i = 0; i < sz && i <= funs->count; i++ )
        free( funs->by_id[i].code );
}

enum vm_load_result 
file_read_funs( FILE* f, struct vm_funs* funs ) {
    assert( funs );

    READVAR( &funs->count );
    funs->by_id = malloc( (funs->count) * sizeof( struct vm_fun ) );

    for( size_t i = 0; i < funs->count; i++ ) {
        enum vm_load_result status;
        if ((status = file_read_fun( f, funs->by_id + i )) != LOAD_OK) {
            free_funs_code( funs, i+1 );
            return status; 
        }
    }
    return LOAD_OK;
} 



enum vm_load_result 
file_load( FILE* f, struct vm_prog* prog ) {
    assert( prog );

    if ( file_check_signature( f ) != LOAD_OK || 
         file_check_version(   f ) != LOAD_OK )
        return LOAD_INVALID_FORMAT;

    struct vm_const_pool new_consts;
    struct vm_funs funs;

    if ( file_read_consts( f, &new_consts ) == LOAD_OK ) {
        enum vm_load_result fun_status;
        if ( (fun_status = file_read_funs( f, &funs )) == LOAD_OK )
            *prog = prog_create( funs,  new_consts );
        else {
            const_pool_deinit( &new_consts ); 
            return fun_status;
        }
    }
    return LOAD_OK; 
}

enum vm_load_result file_load_by_name( 
        const char* name, 
        struct vm_prog* result_prog,
        FILE* err
        ) {
    assert( name ); assert( result_prog );

    struct vm_prog prog;
    FILE* f = fopen( name, "rb" );
    if (! f ) { fprintf( err, "Error opening file %s\n", name ); return LOAD_IO_ERROR; }

    enum vm_load_result load_status = file_load( f, &prog );

    if ( load_status == LOAD_OK ) { 
        fclose( f );
        *result_prog = prog; 
        return LOAD_OK;
    }
    else { 
        fprintf( err, "Error loading file %s\n", name );  
        prog_deinit( &prog ); 
        return load_status;
    } 
}

enum vm_load_result file_load_many(
        char const* const* names, 
        size_t count, 
        struct vm_prog* result,
        FILE* err ){

    struct vm_prog prog; 
    if (count == 0) return LOAD_INVALID_ARGUMENTS;

    enum vm_load_result status;

    status = file_load_by_name( names[0], &prog, err );
    if ( status != LOAD_OK ) return status;

    for( size_t i = 1; i < count; i++ ){
        struct vm_prog current = {0};
        status = file_load_by_name( names[i], &current, err );
        if ( status != LOAD_OK ) {
            prog_deinit( &prog );
            return status;
        } 
        prog = prog_combine_destr( &prog, &current );
    }

    *result = prog;
    return LOAD_OK;
}


/** @} */
