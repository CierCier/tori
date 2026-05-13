#pragma once

#include <stdint.h>
#include <stddef.h>

namespace tori::container {

struct IntrusiveListNode {
    IntrusiveListNode* next;
    IntrusiveListNode* prev;
};

template<typename T, size_t NodeOffset>
class IntrusiveList {
public:
    bool empty() const {
        return head_.next == nullptr || head_.next == &head_;
    }

    void push_back(T* item) {
        auto* node = reinterpret_cast<IntrusiveListNode*>(
            reinterpret_cast<char*>(item) + NodeOffset);
        if (head_.next == nullptr) {
            head_.next = node;
            head_.prev = node;
            node->next = &head_;
            node->prev = &head_;
        } else {
            node->prev = head_.prev;
            node->next = &head_;
            head_.prev->next = node;
            head_.prev = node;
        }
    }

    void push_front(T* item) {
        auto* node = reinterpret_cast<IntrusiveListNode*>(
            reinterpret_cast<char*>(item) + NodeOffset);
        if (head_.next == nullptr) {
            head_.next = node;
            head_.prev = node;
            node->next = &head_;
            node->prev = &head_;
        } else {
            node->next = head_.next;
            node->prev = &head_;
            head_.next->prev = node;
            head_.next = node;
        }
    }

    T* pop_front() {
        if (empty()) return nullptr;
        auto* node = head_.next;
        node->prev->next = node->next;
        node->next->prev = node->prev;
        return reinterpret_cast<T*>(reinterpret_cast<char*>(node) - static_cast<ptrdiff_t>(NodeOffset));
    }

    void remove(T* item) {
        auto* node = reinterpret_cast<IntrusiveListNode*>(
            reinterpret_cast<char*>(item) + NodeOffset);
        node->prev->next = node->next;
        node->next->prev = node->prev;
    }

    class Iterator {
    public:
        explicit Iterator(IntrusiveListNode* pos) : pos_(pos) {}

        T* operator*() const {
            return reinterpret_cast<T*>(reinterpret_cast<char*>(pos_) - static_cast<ptrdiff_t>(NodeOffset));
        }

        Iterator& operator++() { pos_ = pos_->next; return *this; }
        bool operator!=(const Iterator& other) const { return pos_ != other.pos_; }

    private:
        IntrusiveListNode* pos_;
    };

    Iterator begin() {
        return Iterator(head_.next ? head_.next : &head_);
    }

    Iterator end() {
        return Iterator(&head_);
    }

private:
    IntrusiveListNode head_;
};

} // namespace tori::container
