/*
 * SurgeScript
 * A lightweight programming language for computer games and interactive apps
 * Copyright (C) 2017  Alexandre Martins <alemartf(at)gmail(dot)com>
 *
 * util/array.c
 * SurgeScript Arrays
 */

#include "../vm.h"
#include "../heap.h"
#include "../../util/util.h"


/* private stuff */
static surgescript_var_t* fun_constructor(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_destructor(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_main(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_get(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_set(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_length(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_push(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_pop(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_shift(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_unshift(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_sort(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_reverse(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_indexof(surgescript_object_t* object, const surgescript_var_t** param, int num_params);

/* utilities */
#define ORDINAL(j)              (((j) == 1) ? "st" : (((j) == 2) ? "nd" : (((j) == 3) ? "rd" : "th")))
#define ARRAY_LENGTH(heap)      ((int)surgescript_var_get_number(surgescript_heap_at((heap), LENGTH_ADDR)))
static void quicksort(surgescript_heap_t* heap, surgescript_heapptr_t begin, surgescript_heapptr_t end);
static inline surgescript_heapptr_t partition(surgescript_heap_t* heap, surgescript_heapptr_t begin, surgescript_heapptr_t end);
static inline surgescript_var_t* med3(surgescript_var_t* a, surgescript_var_t* b, surgescript_var_t* c);
static const surgescript_heapptr_t LENGTH_ADDR = 0; /* the length of the array is allocated on the first address */
static const surgescript_heapptr_t BASE_ADDR = 1;   /* array elements come later */


/*
 * surgescript_sslib_register_array()
 * Register the methods of the SurgeScript Arrays
 */
void surgescript_sslib_register_array(surgescript_vm_t* vm)
{
    surgescript_vm_bind(vm, "Array", "__constructor", fun_constructor, 0);
    surgescript_vm_bind(vm, "Array", "__destructor", fun_destructor, 0);
    surgescript_vm_bind(vm, "Array", "state:main", fun_main, 0);
    surgescript_vm_bind(vm, "Array", "get", fun_get, 1);
    surgescript_vm_bind(vm, "Array", "set", fun_set, 2);
    surgescript_vm_bind(vm, "Array", "length", fun_length, 0);
    surgescript_vm_bind(vm, "Array", "push", fun_push, 1);
    surgescript_vm_bind(vm, "Array", "pop", fun_pop, 0);
    surgescript_vm_bind(vm, "Array", "shift", fun_shift, 0);
    surgescript_vm_bind(vm, "Array", "unshift", fun_unshift, 1);
    surgescript_vm_bind(vm, "Array", "sort", fun_sort, 0);
    surgescript_vm_bind(vm, "Array", "reverse", fun_reverse, 0);
    surgescript_vm_bind(vm, "Array", "indexOf", fun_indexof, 1);
}


/* my functions */

/* array constructor */
surgescript_var_t* fun_constructor(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    /* since we don't ever free() anything from the heap (except the last cell),
       memory cells are allocated contiguously */
    surgescript_heap_t* heap = surgescript_object_heap(object);

    surgescript_heapptr_t length_addr = surgescript_heap_malloc(heap);
    surgescript_var_set_number(surgescript_heap_at(heap, length_addr), 0);
    ssassert(length_addr == LENGTH_ADDR);

    return surgescript_var_set_objecthandle(surgescript_var_create(), surgescript_object_handle(object));
}

/* destructor */
surgescript_var_t* fun_destructor(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    /* the heap gets freed anyway, so why bother? */
    return NULL;
}

/* main state */
surgescript_var_t* fun_main(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    /* do nothing */
    return NULL;
}

/* returns the length of the array */
surgescript_var_t* fun_length(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    surgescript_heap_t* heap = surgescript_object_heap(object);
    return surgescript_var_clone(surgescript_heap_at(heap, LENGTH_ADDR));
}

/* gets i-th element of the array (indexes are 0-based) */
surgescript_var_t* fun_get(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    surgescript_heap_t* heap = surgescript_object_heap(object);
    int index = surgescript_var_get_number(param[0]);

    if(index >= 0 && index < ARRAY_LENGTH(heap))
        return surgescript_var_clone(surgescript_heap_at(heap, BASE_ADDR + index));

    ssfatal("Can't get %d-%s element of the array: the index is out of bounds.", index, ORDINAL(index));
    return NULL;
}

/* sets the i-th element of the array */
surgescript_var_t* fun_set(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    surgescript_heap_t* heap = surgescript_object_heap(object);
    int index = surgescript_var_get_number(param[0]);
    int length = ARRAY_LENGTH(heap);
    const surgescript_var_t* value = param[1];

    /* sanity check & leak prevention */
    if(index < 0 || index >= length + 1024) {
        ssfatal("Can't set %d-%s element of the array: the index is out of bounds.", index, ORDINAL(index));
        return surgescript_var_clone(value);
    }

    /* create memory addresses as needed */
    while(index >= length) {
        surgescript_heapptr_t ptr = surgescript_heap_malloc(heap); /* fast */
        surgescript_var_set_number(surgescript_heap_at(heap, LENGTH_ADDR), ++length);
        ssassert(ptr == BASE_ADDR + (length - 1));
    }

    /* set the value to the correct address */
    surgescript_var_copy(surgescript_heap_at(heap, BASE_ADDR + index), value);

    /* done! */
    return surgescript_var_clone(value); /* the C expression (arr[i] = value) returns value */
}

/* pushes a new element into the last position of the array */
surgescript_var_t* fun_push(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    surgescript_heap_t* heap = surgescript_object_heap(object);
    const surgescript_var_t* value = param[0];
    int length = ARRAY_LENGTH(heap);

    surgescript_heapptr_t ptr = surgescript_heap_malloc(heap);
    surgescript_var_copy(surgescript_heap_at(heap, ptr), value);
    surgescript_var_set_number(surgescript_heap_at(heap, LENGTH_ADDR), ++length);
    ssassert(ptr == BASE_ADDR + (length - 1));

    return NULL;
}

/* pops the last element from the array */
surgescript_var_t* fun_pop(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    surgescript_heap_t* heap = surgescript_object_heap(object);
    int length = ARRAY_LENGTH(heap);

    if(length > 0) {
        surgescript_var_t* value = surgescript_var_clone(surgescript_heap_at(heap, BASE_ADDR + (length - 1)));
        surgescript_var_set_number(surgescript_heap_at(heap, LENGTH_ADDR), length - 1);
        surgescript_heap_free(heap, BASE_ADDR + (length - 1));
        return value;
    }

    return NULL;
}

/* removes (and returns) the first element and shifts all others to a lower index */
surgescript_var_t* fun_shift(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    surgescript_heap_t* heap = surgescript_object_heap(object);
    int length = ARRAY_LENGTH(heap);

    if(length > 0) {
        surgescript_var_t* value = surgescript_var_clone(surgescript_heap_at(heap, BASE_ADDR + 0));

        for(int i = 0; i < length - 1; i++)
            surgescript_var_copy(surgescript_heap_at(heap, BASE_ADDR + i), surgescript_heap_at(heap, BASE_ADDR + (i + 1)));

        surgescript_var_set_number(surgescript_heap_at(heap, LENGTH_ADDR), length - 1);
        surgescript_heap_free(heap, BASE_ADDR + (length - 1));
        return value;
    }

    return NULL;
}

/* adds an element to the beginning of the array and shifts all others to a higher index */
surgescript_var_t* fun_unshift(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    surgescript_heap_t* heap = surgescript_object_heap(object);
    const surgescript_var_t* value = param[0];
    int length = ARRAY_LENGTH(heap);

    surgescript_heapptr_t ptr = surgescript_heap_malloc(heap);
    surgescript_var_set_number(surgescript_heap_at(heap, LENGTH_ADDR), ++length);
    ssassert(ptr == BASE_ADDR + (length - 1));

    for(int i = length - 1; i > 0; i--)
        surgescript_var_copy(surgescript_heap_at(heap, BASE_ADDR + i), surgescript_heap_at(heap, BASE_ADDR + (i - 1)));
    surgescript_var_copy(surgescript_heap_at(heap, BASE_ADDR + 0), value);

    return NULL;
}

/* reverses the array */
surgescript_var_t* fun_reverse(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    surgescript_heap_t* heap = surgescript_object_heap(object);
    int length = ARRAY_LENGTH(heap);

    for(int i = 0; i < length / 2; i++) {
        surgescript_var_t* a = surgescript_heap_at(heap, BASE_ADDR + i);
        surgescript_var_t* b = surgescript_heap_at(heap, BASE_ADDR + (length - 1 - i));
        surgescript_var_swap(a, b);
    }

    return NULL;
}

/* sorts the array */
surgescript_var_t* fun_sort(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    surgescript_heap_t* heap = surgescript_object_heap(object);
    quicksort(heap, BASE_ADDR, BASE_ADDR + ARRAY_LENGTH(heap) - 1);
    return NULL;
}

/* finds the first i such that array[i] == param[0], or -1 if there is no such a match */
surgescript_var_t* fun_indexof(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    surgescript_heap_t* haystack = surgescript_object_heap(object);
    const surgescript_var_t* needle = param[0];
    int length = ARRAY_LENGTH(haystack);

    for(int i = 0; i < length; i++) {
        surgescript_var_t* element = surgescript_heap_at(haystack, BASE_ADDR + i);
        if(surgescript_var_compare(element, needle) == 0)
            return surgescript_var_set_number(surgescript_var_create(), i);
    }

    return surgescript_var_set_number(surgescript_var_create(), -1);
}


/* utilities */

/* quicksort algorithm: sorts heap[begin .. end] */
void quicksort(surgescript_heap_t* heap, surgescript_heapptr_t begin, surgescript_heapptr_t end)
{
    if(begin < end) {
        surgescript_heapptr_t p = partition(heap, begin, end);
        quicksort(heap, begin, p-1);
        quicksort(heap, p+1, end);
    }
}

/* returns ptr such that heap[begin .. ptr-1] <= heap[ptr] < heap[ptr+1 .. end], where begin <= end */
surgescript_heapptr_t partition(surgescript_heap_t* heap, surgescript_heapptr_t begin, surgescript_heapptr_t end)
{
    surgescript_var_t* pivot = surgescript_heap_at(heap, end);
    surgescript_heapptr_t p = begin;

    surgescript_var_swap(pivot, med3(surgescript_heap_at(heap, begin), surgescript_heap_at(heap, begin + (end-begin)/2), pivot));
    for(surgescript_heapptr_t i = begin; i <= end - 1; i++) {
        if(surgescript_var_compare(surgescript_heap_at(heap, i), pivot) <= 0) {
            surgescript_var_swap(surgescript_heap_at(heap, i), surgescript_heap_at(heap, p));
            p++;
        }
    }

    surgescript_var_swap(surgescript_heap_at(heap, p), pivot);
    return p;
}

/* returns the median of 3 variables */
surgescript_var_t* med3(surgescript_var_t* a, surgescript_var_t* b, surgescript_var_t* c)
{
    int ab = surgescript_var_compare(a, b);
    int bc = surgescript_var_compare(b, c);
    int ac = surgescript_var_compare(a, c);

    if(ab >= 0 && ac >= 0) /* a = max(a, b, c) */
        return bc >= 0 ? b : c;
    else if(ab <= 0 && bc >= 0) /* b = max(a, b, c) */
        return ac >= 0 ? a : c;
    else /* c = max(a, b, c) */
        return ab >= 0 ? a : b;
}