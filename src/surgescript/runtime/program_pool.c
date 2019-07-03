/*
 * SurgeScript
 * A scripting language for games
 * Copyright 2016-2018 Alexandre Martins <alemartf(at)gmail(dot)com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * runtime/program_pool.c
 * SurgeScript program pool
 */

#include <stdint.h>
#include <string.h>
#include "program_pool.h"
#include "program.h"
#include "../util/uthash.h"
#include "../util/util.h"
#include "../util/ssarray.h"

/* options */
#define SURGESCRIPT_USE_FAST_SIGNATURES /* let it be fast! */

/*
 * Each function in SurgeScript defines a function signature
 * that depends on the containing object and on the function name
 */

#ifdef SURGESCRIPT_USE_FAST_SIGNATURES /* use fast 64-bit integers for function signatures */
typedef uint64_t surgescript_programpool_signature_t;
#define HASH_FIND_ITEM_BY_SIGNATURE(head, signature, item_ptr)     HASH_FIND(hh, (head), &(signature), sizeof(signature), (item_ptr))
#define HASH_ADD_ITEM_WITH_SIGNATURE(head, signature, item_ptr)    HASH_ADD_KEYPTR(hh, (head), &(signature), sizeof(signature), (item_ptr))
#else /* use strings for function signatures */
typedef char* surgescript_programpool_signature_t;
#define HASH_FIND_ITEM_BY_SIGNATURE(head, signature, item_ptr)     HASH_FIND(hh, (head), (signature), strlen(signature), (item_ptr))
#define HASH_ADD_ITEM_WITH_SIGNATURE(head, signature, item_ptr)    HASH_ADD_KEYPTR(hh, (head), (signature), strlen(signature), (item_ptr))
#endif

static inline surgescript_programpool_signature_t generate_signature(const char* object_name, const char* program_name); /* generates a function signature, given an object name and a program name */
static inline void delete_signature(surgescript_programpool_signature_t signature); /* deletes a signature */
extern void hashlittle2(const void *key, size_t length, uint32_t* pc, uint32_t* pb);

/* metadata */
typedef struct surgescript_programpool_metadata_t surgescript_programpool_metadata_t;
struct surgescript_programpool_metadata_t /* we'll store the list of program names for each object */
{
    char* object_name;
    SSARRAY(char*, program_name);
    UT_hash_handle hh;
};

static void insert_metadata(surgescript_programpool_t* pool, const char* object_name, const char* program_name);
static void remove_metadata(surgescript_programpool_t* pool, const char* object_name, const char* program_name);
static void remove_object_metadata(surgescript_programpool_t* pool, const char* object_name);
static void clear_metadata(surgescript_programpool_t* pool);
static void traverse_metadata(surgescript_programpool_t* pool, const char* object_name, void* data, void (*callback)(const char*,void*));
static void traverse_adapter(const char* program_name, void* callback);


/* program pool hash type */
typedef struct surgescript_programpool_hashpair_t surgescript_programpool_hashpair_t;
struct surgescript_programpool_hashpair_t /* for each function signature, store a reference to its program */
{
    surgescript_programpool_signature_t signature; /* key */
    surgescript_program_t* program; /* value */
    UT_hash_handle hh;
};


/* program pool */
struct surgescript_programpool_t
{
    surgescript_programpool_hashpair_t* hash;
    surgescript_programpool_metadata_t* meta;
};

static void delete_program(const char* program_name, void* data);




/* -------------------------------
 * public methods
 * ------------------------------- */

/*
 * surgescript_programpool_create()
 * Creates a new program pool
 */
surgescript_programpool_t* surgescript_programpool_create()
{
    surgescript_programpool_t* pool = ssmalloc(sizeof *pool);
    pool->hash = NULL;
    pool->meta = NULL;
    return pool;
}

/*
 * surgescript_programpool_destroy()
 * Destroys a program pool
 */
surgescript_programpool_t* surgescript_programpool_destroy(surgescript_programpool_t* pool)
{
    surgescript_programpool_hashpair_t *it, *tmp;

    HASH_ITER(hh, pool->hash, it, tmp) {
        HASH_DEL(pool->hash, it);
        delete_signature(it->signature);
        surgescript_program_destroy(it->program);
        ssfree(it);
    }

    clear_metadata(pool);
    return ssfree(pool);
}

/*
 * surgescript_programpool_exists()
 * Does the specified program exist in the pool?
 */
bool surgescript_programpool_exists(surgescript_programpool_t* pool, const char* object_name, const char* program_name)
{
    return surgescript_programpool_get(pool, object_name, program_name) != NULL;
}

/*
 * surgescript_programpool_shallowcheck()
 * Does the specified program, of EXACTLY the specified object (not a parent class), exist in the pool?
 */
bool surgescript_programpool_shallowcheck(surgescript_programpool_t* pool, const char* object_name, const char* program_name)
{
    surgescript_programpool_hashpair_t* pair = NULL;
    surgescript_programpool_signature_t signature = generate_signature(object_name, program_name);

    HASH_FIND_ITEM_BY_SIGNATURE(pool->hash, signature, pair);
    delete_signature(signature);

    return pair != NULL;
}

/*
 * surgescript_programpool_put()
 * Puts a program in the pool
 */
bool surgescript_programpool_put(surgescript_programpool_t* pool, const char* object_name, const char* program_name, surgescript_program_t* program)
{
    if(!surgescript_programpool_shallowcheck(pool, object_name, program_name)) {
        surgescript_programpool_hashpair_t* pair = ssmalloc(sizeof *pair);
        pair->signature = generate_signature(object_name, program_name);
        pair->program = program;
        HASH_ADD_ITEM_WITH_SIGNATURE(pool->hash, pair->signature, pair);
        insert_metadata(pool, object_name, program_name);
        return true;
    }
    else {
        ssfatal("Runtime Error: duplicate function \"%s\" in object \"%s\"", program_name, object_name);
        return false;
    }
}


/*
 * surgescript_programpool_get()
 * Gets a program from the pool (returns NULL if not found)
 * This needs to be fast!
 */
surgescript_program_t* surgescript_programpool_get(surgescript_programpool_t* pool, const char* object_name, const char* program_name)
{
    surgescript_programpool_hashpair_t* pair = NULL;
    surgescript_programpool_signature_t signature = generate_signature(object_name, program_name);
    
    /* find the program */
    HASH_FIND_ITEM_BY_SIGNATURE(pool->hash, signature, pair);
    delete_signature(signature);

    /* if there is no such program */
    if(!pair) {
        /* try locating it in a common base for all objects */
        signature = generate_signature("Object", program_name);
        HASH_FIND_ITEM_BY_SIGNATURE(pool->hash, signature, pair);
        delete_signature(signature);

        /* really, the program doesn't exist */
        if(!pair)
            return NULL;
    }

    /* found it! */
    return pair->program;
}

/*
 * surgescript_programpool_foreach()
 * For each program of object_name, calls the callback
 */
void surgescript_programpool_foreach(surgescript_programpool_t* pool, const char* object_name, void (*callback)(const char*))
{
    traverse_metadata(pool, object_name, callback, traverse_adapter);
}

/*
 * surgescript_programpool_foreach_ex()
 * For each program of object_name, calls the callback with an added data parameter
 */
void surgescript_programpool_foreach_ex(surgescript_programpool_t* pool, const char* object_name, void* data, void (*callback)(const char*, void*))
{
    traverse_metadata(pool, object_name, data, callback);
}



/*
 * surgescript_programpool_replace()
 * Replaces a program in the pool (if it doesn't exist, create it)
 */
bool surgescript_programpool_replace(surgescript_programpool_t* pool, const char* object_name, const char* program_name, surgescript_program_t* program)
{
    surgescript_programpool_hashpair_t* pair = NULL;
    surgescript_programpool_signature_t signature = generate_signature(object_name, program_name);
    
    /* find the program */
    HASH_FIND_ITEM_BY_SIGNATURE(pool->hash, signature, pair);
    delete_signature(signature);

    /* replace the program */
    if(pair != NULL) {
        surgescript_program_destroy(pair->program);
        pair->program = program;
        return true;
    }
    else
        return surgescript_programpool_put(pool, object_name, program_name, program); /* program doesn't exist yet */
}


/*
 * surgescript_programpool_purge()
 * Deletes all the programs of the specified object
 */
void surgescript_programpool_purge(surgescript_programpool_t* pool, const char* object_name)
{
    void* data[] = { pool, (void*)object_name };
    surgescript_programpool_foreach_ex(pool, object_name, data, delete_program);
    remove_object_metadata(pool, object_name);
}


/*
 * surgescript_programpool_delete()
 * Deletes a programs from the specified object
 */
void surgescript_programpool_delete(surgescript_programpool_t* pool, const char* object_name, const char* program_name)
{
    surgescript_programpool_hashpair_t* pair = NULL;
    surgescript_programpool_signature_t signature = generate_signature(object_name, program_name);
    
    /* find the program */
    HASH_FIND_ITEM_BY_SIGNATURE(pool->hash, signature, pair);
    delete_signature(signature);

    /* delete the program */
    if(pair != NULL) {
        HASH_DEL(pool->hash, pair);
        delete_signature(pair->signature);
        surgescript_program_destroy(pair->program);
        ssfree(pair);
    }

    /* delete metadata */
    remove_metadata(pool, object_name, program_name);
}



/*
 * surgescript_programpool_is_compiled()
 * Is there any code for object_name?
 */
bool surgescript_programpool_is_compiled(surgescript_programpool_t* pool, const char* object_name)
{
    surgescript_programpool_metadata_t *m = NULL;
    HASH_FIND_STR(pool->meta, object_name, m);
    return (m != NULL) && (ssarray_length(m->program_name) > 0);
}



/* -------------------------------
 * private methods
 * ------------------------------- */


 /* metadata */
void insert_metadata(surgescript_programpool_t* pool, const char* object_name, const char* program_name)
{
    surgescript_programpool_metadata_t *m = NULL;
    HASH_FIND(hh, pool->meta, object_name, strlen(object_name), m);

    /* create the hash entry if it doesn't exist yet */
    if(m == NULL) {
        m = ssmalloc(sizeof *m);
        m->object_name = ssstrdup(object_name);
        ssarray_init(m->program_name);
        HASH_ADD_KEYPTR(hh, pool->meta, m->object_name, strlen(m->object_name), m);
    }

    /* no need to check for key uniqueness (it's checked before) */
    ssarray_push(m->program_name, ssstrdup(program_name));
}

void remove_metadata(surgescript_programpool_t* pool, const char* object_name, const char* program_name)
{
    surgescript_programpool_metadata_t *m = NULL;
    HASH_FIND(hh, pool->meta, object_name, strlen(object_name), m);

    if(m != NULL) {
        int index = -1;

        /* find the program */
        for(int i = 0; i < ssarray_length(m->program_name) && index < 0; i++) {
            if(strcmp(m->program_name[i], program_name) == 0)
                index = i;
        }

        /* delete the program */
        if(index >= 0) {
            ssfree(m->program_name[index]);
            ssarray_remove(m->program_name, index);
        }
    }
}

void remove_object_metadata(surgescript_programpool_t* pool, const char* object_name)
{
    surgescript_programpool_metadata_t *m = NULL;
    HASH_FIND(hh, pool->meta, object_name, strlen(object_name), m);

    /* delete all programs of object_name */
    if(m != NULL) {
        HASH_DEL(pool->meta, m);
        for(int i = 0; i < ssarray_length(m->program_name); i++)
            ssfree(m->program_name[i]);
        ssarray_release(m->program_name);
        ssfree(m->object_name);
        ssfree(m);
    }
}

void clear_metadata(surgescript_programpool_t* pool)
{
    surgescript_programpool_metadata_t *it, *tmp;

    /* delete all programs of all objects */
    HASH_ITER(hh, pool->meta, it, tmp) {
        HASH_DEL(pool->meta, it);
        for(int i = 0; i < ssarray_length(it->program_name); i++)
            ssfree(it->program_name[i]);
        ssarray_release(it->program_name);
        ssfree(it->object_name);
        ssfree(it);
    }
}

void traverse_metadata(surgescript_programpool_t* pool, const char* object_name, void* data, void (*callback)(const char*,void*))
{
    surgescript_programpool_metadata_t *m = NULL;
    HASH_FIND_STR(pool->meta, object_name, m);

    if(m != NULL) {
        for(int i = 0; i < ssarray_length(m->program_name); i++)
            callback(m->program_name[i], data);
    }
}

void traverse_adapter(const char* program_name, void* callback)
{
    ((void (*)(const char*))callback)(program_name);
}


/* utilities */
void delete_program(const char* program_name, void* data)
{
    surgescript_programpool_t* pool = (surgescript_programpool_t*)(((void**)data)[0]);
    const char* object_name = (const char*)(((void**)data)[1]);
    surgescript_programpool_signature_t signature = generate_signature(object_name, program_name);
    surgescript_programpool_hashpair_t* pair = NULL;

    /* find the program */
    HASH_FIND_ITEM_BY_SIGNATURE(pool->hash, signature, pair);   
    delete_signature(signature);

    /* delete the program */
    if(pair != NULL) {
        HASH_DEL(pool->hash, pair);
        delete_signature(pair->signature);
        surgescript_program_destroy(pair->program);
        ssfree(pair);
    }
}


/* function signature methods */
#ifdef SURGESCRIPT_USE_FAST_SIGNATURES
surgescript_programpool_signature_t generate_signature(const char* object_name, const char* program_name)
{
    /* uthash will compute a hash of this hash. Our app must enforce key uniqueness. */
    uint32_t pc = 0, pb = 0;
    hashlittle2(object_name, strlen(object_name), &pc, &pb); /* strlen() is reasonably fast */
    hashlittle2(program_name, strlen(program_name), &pc, &pb);
    return (uint64_t)pc | (((uint64_t)pb) << 32);
}

void delete_signature(surgescript_programpool_signature_t signature)
{
    ;
}
#else
surgescript_programpool_signature_t generate_signature(const char* object_name, const char* program_name)
{
    size_t olen = strlen(object_name);
    char* signature = ssmalloc((olen + strlen(program_name) + 2) * sizeof(*signature));
    strcpy(signature, object_name);
    signature[olen] = '\036';
    strcpy(signature + olen + 1, program_name);
    return signature;
}

void delete_signature(surgescript_programpool_signature_t signature)
{
    ssfree(signature);
}
#endif
