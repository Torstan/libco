#pragma once

#include <cassert>
#include <type_traits>

template <class T> class LinkedList;
// A doubly-linked intrusive list.
//
// Requirements for T:
//   T*              prev
//   T*              next
//   LinkedList<T>*  link   -- null when not in any list
template <typename T> struct LinkItemBase {
  T *prev{nullptr};
  T *next{nullptr};
  LinkedList<T> *link{nullptr};
};

template <class T> class LinkedList {
  // Ensure T inherits from LinkItemBase<T>
  static_assert(std::is_base_of<LinkItemBase<T>, T>::value,
                "T must inherit from LinkItemBase<T>");

public:
  T *head = nullptr;
  T *tail = nullptr;

  bool empty() const { return head == nullptr; }
  void clear() { head = tail = nullptr; }

  // Add node to the tail. No-op if node is already in a list.
  void add_tail(T *node) {
    if (node->link)
      return;
    if (tail) {
      tail->next = node;
      node->prev = tail;
      node->next = nullptr;
      tail = node;
    } else {
      head = tail = node;
      node->prev = node->next = nullptr;
    }
    node->link = this;
  }

  // Pop the head node and return it. Caller owns the memory.
  // Returns nullptr if the list is empty.
  T *pop_head() {
    if (!head)
      return nullptr;
    T *node = head;
    if (head == tail) {
      head = tail = nullptr;
    } else {
      head = head->next;
      head->prev = nullptr;
    }
    node->prev = node->next = nullptr;
    node->link = nullptr;
    return node;
  }

  // Append all nodes from other into this list. other becomes empty.
  void join(LinkedList &other) {
    if (!other.head)
      return;
    for (T *p = other.head; p; p = p->next)
      p->link = this;
    if (tail) {
      tail->next = other.head;
      other.head->prev = tail;
      tail = other.tail;
    } else {
      head = other.head;
      tail = other.tail;
    }
    other.head = other.tail = nullptr;
  }

  // Remove node from its owning list. No-op if node is not in any list.
  static void remove(T *node) {
    LinkedList *lst = node->link;
    if (!lst)
      return;
    assert(lst->head && lst->tail);

    if (node == lst->head) {
      lst->head = node->next;
      if (lst->head)
        lst->head->prev = nullptr;
    } else {
      node->prev->next = node->next;
    }

    if (node == lst->tail) {
      lst->tail = node->prev;
      if (lst->tail)
        lst->tail->next = nullptr;
    } else {
      node->next->prev = node->prev;
    }

    node->prev = node->next = nullptr;
    node->link = nullptr;
  }
};
