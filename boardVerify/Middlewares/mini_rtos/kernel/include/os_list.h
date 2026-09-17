/**
 * @file os_list.h
 * @author Peng RongYao
 * @date 2026-08-22
 * @brief 定义通用内核侵入式双向链表类型并声明链表操作。
 */

#ifndef MINI_RTOS_OS_LIST_H
#define MINI_RTOS_OS_LIST_H

#include "os.h"

#include <stdbool.h>
#include <stddef.h>

/* TCB 的前向声明，链表模块不拥有 TCB 的具体定义。 */
struct os_task;

/* TCB 内嵌的侵入式双向链表节点。 */
typedef struct os_list_node {
    struct os_list_node *previous; /* 前一个节点，表头为 NULL。 */
    struct os_list_node *next;     /* 后一个节点，表尾为 NULL。 */
    struct os_task *owner;         /* 拥有该节点的 TCB。 */
    bool linked;                   /* 节点当前是否属于某一条内核链表。 */
} os_list_node_t;

/* 就绪、延时和对象等待队列共用的链表头。 */
typedef struct {
    os_list_node_t *head; /* 表头节点。 */
    os_list_node_t *tail; /* 表尾节点。 */
    size_t size;          /* 当前节点数量。 */
} os_list_t;

/* 将链表初始化为空表。 */
void os_ListInit(os_list_t *list);
/* 将未链接节点追加到表尾。 */
void os_ListPushBack(os_list_t *list, os_list_node_t *node);
/**
 * @brief 在指定节点之前插入一个未链接节点。
 * @param list 目标链表。
 * @param position 已属于 list 的定位节点。
 * @param node 待插入的节点。
 */
void os_ListInsertBefore(
    os_list_t *list,
    os_list_node_t *position,
    os_list_node_t *node
);
/* 弹出并返回表头节点；空表返回 NULL。 */
os_list_node_t *os_ListPopFront(os_list_t *list);
/* 从链表移除指定节点。 */
void os_ListRemove(os_list_t *list, os_list_node_t *node);
/* 判断节点是否属于给定链表。 */
bool os_ListContains(const os_list_t *list, const os_list_node_t *node);

#endif
