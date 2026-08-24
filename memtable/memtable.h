#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

enum class EntryType { Live, Tombstone };

struct MemtableEntry {
    std::string key;
    std::string value;
    EntryType type;
};

class Memtable {
public:
    Memtable();
    ~Memtable();

    Memtable(const Memtable&) = delete;
    Memtable& operator=(const Memtable&) = delete;

    void put(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key) const;
    bool contains(const std::string& key) const;
    void remove(const std::string& key);  // tombstone, not physical delete

    void forEach(const std::function<void(const MemtableEntry&)>& fn) const;
    std::size_t size() const;

    Memtable(Memtable&& other) noexcept;
    Memtable& operator=(Memtable&& other) noexcept;

private:
    struct Node;

    Node* root_;
    std::size_t size_;

    Node* findNode(const std::string& key) const;
    Node* insertNode(const std::string& key, const std::string& value, EntryType type);
    void fixInsert(Node* node);
    void rotateLeft(Node* node);
    void rotateRight(Node* node);
    void destroyTree(Node* node);
    void inorderVisit(Node* node,
                      const std::function<void(const MemtableEntry&)>& fn) const;
};