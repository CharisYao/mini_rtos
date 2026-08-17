/**
 * @file os_list.c
 * @author Peng RongYao
 * @date 2026-07-17
 * @brief 实现任务就绪、延时和对象等待队列共用的侵入式双向链表。
 *
 * @details
 * 链表节点直接嵌入 TCB，内核移动任务时只调整节点关系，不申请或释放动态内存。
 * 本文件提供链表初始化、尾部追加、指定位置插入、表头弹出、节点删除和成员检查。
 *
 * 就绪队列借助尾部追加和表头弹出实现同优先级 FIFO；信号量、队列和互斥量的
 * 等待队列则借助指定位置插入保持优先级顺序。成员检查采用受限遍历，避免链表
 * 被破坏后测试或不变量检查陷入无限循环。
 */

#include "os_internal.h"

#include <assert.h>

/* 将链表复位为空；节点本身由各 TCB 持有，不在这里分配。 */
void os_ListInit(os_list_t *list)
{
    assert(list != NULL);
    list->head = NULL;
    list->tail = NULL;
    list->size = 0U;
}

/**
 * @brief 将一个未链接节点追加到链表尾部。
 * @param list 目标链表。
 * @param node 待插入节点；已属于其他链表时触发断言。
 */
void os_ListPushBack(os_list_t *list, os_list_node_t *node)
{
    assert(list != NULL);
    assert(node != NULL);
    assert(!node->linked);

    node->previous = list->tail;
    node->next = NULL;

    if (list->tail != NULL) {
        list->tail->next = node;
    } else {
        list->head = node;
    }

    list->tail = node;
    node->linked = true;
    list->size++;
}

/**
 * @brief 在 position 前插入一个未链接节点。
 * @param list 目标链表。
 * @param position 必须已经属于 list 的定位节点。
 * @param node 待插入节点。
 */
void os_ListInsertBefore(
    os_list_t *list,
    os_list_node_t *position,
    os_list_node_t *node
)
{
    assert(list != NULL);
    assert(position != NULL);
    assert(position->linked);
    assert(node != NULL);
    assert(!node->linked);

    node->previous = position->previous;
    node->next = position;

    if (position->previous != NULL) {
        position->previous->next = node;
    } else {
        assert(list->head == position);
        list->head = node;
    }

    position->previous = node;
    node->linked = true;
    list->size++;
}

/* 弹出表头并解除节点的全部链表关系。 */
os_list_node_t *os_ListPopFront(os_list_t *list)
{
    os_list_node_t *node;

    assert(list != NULL);

    node = list->head;
    if (node == NULL) {
        return NULL;
    }

    list->head = node->next;
    if (list->head != NULL) {
        list->head->previous = NULL;
    } else {
        list->tail = NULL;
    }

    node->previous = NULL;
    node->next = NULL;
    node->linked = false;
    list->size--;
    return node;
}

/**
 * @brief 从链表中移除指定节点。
 * @param list 节点当前所在的链表。
 * @param node 待移除节点，必须处于已链接状态。
 */
void os_ListRemove(os_list_t *list, os_list_node_t *node)
{
    assert(list != NULL);
    assert(node != NULL);
    assert(node->linked);
    assert(list->size > 0U);

    if (node->previous != NULL) {
        node->previous->next = node->next;
    } else {
        assert(list->head == node);
        list->head = node->next;
    }

    if (node->next != NULL) {
        node->next->previous = node->previous;
    } else {
        assert(list->tail == node);
        list->tail = node->previous;
    }

    node->previous = NULL;
    node->next = NULL;
    node->linked = false;
    list->size--;
}

/**
 * @brief 以受限遍历判断节点是否确实属于给定链表。
 * @param list 待检查链表。
 * @param node 目标节点。
 * @return 找到目标节点返回 true；参数无效、未找到或疑似成环时返回 false。
 */
bool os_ListContains(const os_list_t *list, const os_list_node_t *node)
{
    const os_list_node_t *current;
    size_t visited = 0U;

    if ((list == NULL) || (node == NULL)) {
        return false;
    }

    /* 链表最多包含 OS_MAX_TASKS 个 TCB，遍历上限还能防止损坏后死循环。 */
    current = list->head;
    while ((current != NULL) && (visited <= OS_MAX_TASKS)) {
        if (current == node) {
            return true;
        }
        current = current->next;
        visited++;
    }

    return false;
}
