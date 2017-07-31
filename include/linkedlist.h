#ifndef LINKEDLIST_H
#define LINKEDLIST_H

#include <stdio.h>
#include <stdlib.h>

#define STR(QN) #QN

// wrapper element of the list
#define ELEMENT(QN) QN ## _elem

// freelist of the list
#define FREELIST(QN) QN ## _freelist

#define DECLARE_LINKED_LIST(T_ELEM, ZERO_VALUE, QUEUE_NAME)                                        \
typedef struct ELEMENT(QUEUE_NAME) {                                                               \
  T_ELEM                       elem;                                                               \
  struct ELEMENT(QUEUE_NAME)   *next;                                                              \
} ELEMENT(QUEUE_NAME);                                                                             \
                                                                                                   \
typedef struct FREELIST(QUEUE_NAME) {                                                              \
  ELEMENT(QUEUE_NAME)          *list;                                                              \
  ELEMENT(QUEUE_NAME)          *head;                                                              \
  ELEMENT(QUEUE_NAME)          *tail;                                                              \
} FREELIST(QUEUE_NAME);                                                                            \
                                                                                                   \
typedef struct QUEUE_NAME {                                                                        \
  FREELIST(QUEUE_NAME)  freelist;                                                                  \
  ELEMENT(QUEUE_NAME)   *head;                                                                     \
  ELEMENT(QUEUE_NAME)   *tail;                                                                     \
} QUEUE_NAME;                                                                                      \
                                                                                                   \
static inline void QUEUE_NAME ## _init(QUEUE_NAME* q, int pool_size)                               \
{                                                                                                  \
  int i;                                                                                           \
  pool_size += 2;                                                                                  \
  q->freelist.list = (ELEMENT(QUEUE_NAME)*) malloc((pool_size) * sizeof(ELEMENT(QUEUE_NAME)));     \
  for(i=1; i<pool_size-1; i++) {                                                                   \
    q->freelist.list[i].elem = ZERO_VALUE;                                                         \
    q->freelist.list[i].next = &q->freelist.list[i+1];                                             \
  }                                                                                                \
  /* first elem as sentinel */                                                                     \
  q->freelist.list[0].elem           = ZERO_VALUE;                                                 \
  q->freelist.list[0].next           = NULL;                                                       \
  q->freelist.list[pool_size-1].elem = ZERO_VALUE;                                                 \
  q->freelist.list[pool_size-1].next = NULL;                                                       \
  q->freelist.head                   = &q->freelist.list[1];                                       \
  q->freelist.tail                   = &q->freelist.list[pool_size-1];                             \
  q->head                            = &q->freelist.list[0];                                       \
  q->tail                            = &q->freelist.list[0];                                       \
}                                                                                                  \
                                                                                                   \
static inline void QUEUE_NAME ## _clear(QUEUE_NAME* q)                                             \
{                                                                                                  \
  ELEMENT(QUEUE_NAME)* first = q->head->next;                                                      \
  if(first) {                                                                                      \
    ELEMENT(QUEUE_NAME)* last = q->tail;                                                           \
    q->freelist.tail->next = first;                                                                \
    q->freelist.tail       = last;                                                                 \
  }                                                                                                \
  q->head->next = NULL;                                                                            \
  q->tail       = q->head;                                                                         \
}                                                                                                  \
                                                                                                   \
static inline void QUEUE_NAME ## _enqueue(QUEUE_NAME* q, T_ELEM v)                                 \
{                                                                                                  \
  if(q->freelist.head == q->freelist.tail) {                                                       \
    printf("linked list %s full!\n", STR(QUEUE_NAME));                                             \
    exit(1);                                                                                       \
  }                                                                                                \
  ELEMENT(QUEUE_NAME)* free = q->freelist.head->next;                                              \
  q->freelist.head->next = free->next;                                                             \
  if(free->next == NULL) {                                                                         \
    q->freelist.tail = q->freelist.head;                                                           \
  }                                                                                                \
  free->elem    = v;                                                                               \
  free->next    = NULL;                                                                            \
  q->tail->next = free;                                                                            \
  q->tail       = free;                                                                            \
}                                                                                                  \
                                                                                                   \
static inline T_ELEM QUEUE_NAME ## _dequeue(QUEUE_NAME* q)                                         \
{                                                                                                  \
  T_ELEM v;                                                                                        \
  ELEMENT(QUEUE_NAME)* elem;                                                                       \
  if(q->head == q->tail) {                                                                         \
    return ZERO_VALUE;                                                                             \
  }                                                                                                \
  elem = q->head->next;                                                                            \
  v    = elem->elem;                                                                               \
  q->head->next = elem->next;                                                                      \
  if(elem->next == NULL) {                                                                         \
    q->tail = q->head;                                                                             \
  }                                                                                                \
  elem->elem = ZERO_VALUE;                                                                         \
  elem->next = NULL;                                                                               \
  q->freelist.tail->next = elem;                                                                   \
  q->freelist.tail       = elem;                                                                   \
  return v;                                                                                        \
}                                                                                                  \
                                                                                                   \
static inline int QUEUE_NAME ## _dequeue_inplace(QUEUE_NAME* q, T_ELEM* v)                         \
{                                                                                                  \
  ELEMENT(QUEUE_NAME)* elem;                                                                       \
  if(q->head == q->tail) {                                                                         \
    return -1;                                                                                     \
  }                                                                                                \
  elem = q->head->next;                                                                            \
  (*v) = elem->elem;                                                                               \
  q->head->next = elem->next;                                                                      \
  if(elem->next == NULL) {                                                                         \
    q->tail = q->head;                                                                             \
  }                                                                                                \
  elem->elem = ZERO_VALUE;                                                                         \
  elem->next = NULL;                                                                               \
  q->freelist.tail->next = elem;                                                                   \
  q->freelist.tail       = elem;                                                                   \
  return 0;                                                                                        \
}                                                                                                  \

#endif