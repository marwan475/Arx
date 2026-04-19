#ifndef KLIB_INTRUSIVE_LIST_H
#define KLIB_INTRUSIVE_LIST_H

#include <stddef.h>

// doubly linked intrusive list macros
// work on structs next and prev pointers
// no allocations

// Initialize a detached intrusive node by clearing link pointers.
#define ILIST_NODE_INIT(node)        \
    do                               \
    {                                \
        (node)->next = NULL;         \
        (node)->prev = NULL;         \
    } while (0)

// Insert node at the front of the list and update the list head.
#define ILIST_PUSH_FRONT(head, node) \
    do                               \
    {                                \
        __typeof__(head) _node = (node);   \
        _node->prev            = NULL;      \
        _node->next            = (head);    \
        if ((head) != NULL)               \
        {                                 \
            (head)->prev = _node;         \
        }                                 \
        (head) = _node;                   \
    } while (0)

// Unlink node from the list, fix neighbors/head, then detach node links.
#define ILIST_REMOVE(head, node)     \
    do                               \
    {                                \
        __typeof__(head) _node = (node);   \
        if (_node->prev != NULL)           \
        {                                  \
            _node->prev->next = _node->next; \
        }                                  \
        else                               \
        {                                  \
            (head) = _node->next;          \
        }                                  \
        if (_node->next != NULL)           \
        {                                  \
            _node->next->prev = _node->prev; \
        }                                  \
        ILIST_NODE_INIT(_node);            \
    } while (0)

// Insert node immediately before pos; if pos is head, update head.
#define ILIST_INSERT_BEFORE(head, pos, node) \
    do                                       \
    {                                        \
        __typeof__(head) _pos  = (pos);      \
        __typeof__(head) _node = (node);     \
        _node->next            = _pos;       \
        _node->prev            = _pos->prev; \
        if (_pos->prev != NULL)              \
        {                                    \
            _pos->prev->next = _node;        \
        }                                    \
        else                                 \
        {                                    \
            (head) = _node;                  \
        }                                    \
        _pos->prev = _node;                  \
    } while (0)

// Append node to the end of the list.
#define ILIST_APPEND(head, node)     \
    do                               \
    {                                \
        __typeof__(head) _node = (node);   \
        ILIST_NODE_INIT(_node);            \
        if ((head) == NULL)                \
        {                                  \
            (head) = _node;                \
        }                                  \
        else                               \
        {                                  \
            __typeof__(head) _tail = (head); \
            while (_tail->next != NULL)      \
            {                                \
                _tail = _tail->next;         \
            }                                \
            _tail->next = _node;             \
            _node->prev = _tail;             \
        }                                  \
    } while (0)

// Iterate list from head to tail.
#define ILIST_FOR_EACH(iter, head) \
    for (__typeof__(head) iter = (head); iter != NULL; iter = iter->next)

// Iterate safely when current node may be removed during traversal.
#define ILIST_FOR_EACH_SAFE(iter, next_iter, head) \
    for (__typeof__(head) iter = (head), next_iter = (iter != NULL ? iter->next : NULL); iter != NULL; iter = next_iter, next_iter = (iter != NULL ? iter->next : NULL))

#endif