#include "memtable.h"

struct Memtable::Node {
    std::string key;
    std::string value;
    EntryType type;
    Node* left;
    Node* right;
    Node* parent;
    bool red;

    Node(std::string k, std::string v, EntryType t)
        : key(std::move(k)),
          value(std::move(v)),
          type(t),
          left(nullptr),
          right(nullptr),
          parent(nullptr),
          red(true) {}
};

Memtable::Memtable() : root_(nullptr), size_(0) {}

Memtable::~Memtable() {
    destroyTree(root_);
}

std::size_t Memtable::size() const {
    return size_;
}

Memtable::Node* Memtable::findNode(const std::string& key) const {
    Node* current = root_;
    while (current) {
        if (key < current->key) {
            current = current->left;
        } else if (key > current->key) {
            current = current->right;
        } else {
            return current;
        }
    }
    return nullptr;
}

void Memtable::rotateLeft(Node* x) {
    Node* y = x->right;
    x->right = y->left;
    if (y->left) {
        y->left->parent = x;
    }

    y->parent = x->parent;
    if (!x->parent) {
        root_ = y;
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }

    y->left = x;
    x->parent = y;
}

void Memtable::rotateRight(Node* y) {
    Node* x = y->left;
    y->left = x->right;
    if (x->right) {
        x->right->parent = y;
    }

    x->parent = y->parent;
    if (!y->parent) {
        root_ = x;
    } else if (y == y->parent->left) {
        y->parent->left = x;
    } else {
        y->parent->right = x;
    }

    x->right = y;
    y->parent = x;
}

void Memtable::fixInsert(Node* z) {
    while (z->parent && z->parent->red) {
        Node* parent = z->parent;
        Node* grandparent = parent->parent;

        if (parent == grandparent->left) {
            Node* uncle = grandparent->right;
            if (uncle && uncle->red) {
                parent->red = false;
                uncle->red = false;
                grandparent->red = true;
                z = grandparent;
            } else {
                if (z == parent->right) {
                    z = parent;
                    rotateLeft(z);
                    parent = z->parent;
                    grandparent = parent->parent;
                }
                parent->red = false;
                grandparent->red = true;
                rotateRight(grandparent);
            }
        } else {
            Node* uncle = grandparent->left;
            if (uncle && uncle->red) {
                parent->red = false;
                uncle->red = false;
                grandparent->red = true;
                z = grandparent;
            } else {
                if (z == parent->left) {
                    z = parent;
                    rotateRight(z);
                    parent = z->parent;
                    grandparent = parent->parent;
                }
                parent->red = false;
                grandparent->red = true;
                rotateLeft(grandparent);
            }
        }
    }
    root_->red = false;
}

Memtable::Node* Memtable::insertNode(const std::string& key,
                                     const std::string& value,
                                     EntryType type) {
    Node* node = new Node(key, value, type);
    Node* parent = nullptr;
    Node* current = root_;

    while (current) {
        parent = current;
        if (key < current->key) {
            current = current->left;
        } else if (key > current->key) {
            current = current->right;
        } else {
            current->value = value;
            current->type = type;
            delete node;
            return current;
        }
    }

    node->parent = parent;
    if (!parent) {
        root_ = node;
    } else if (key < parent->key) {
        parent->left = node;
    } else {
        parent->right = node;
    }

    fixInsert(node);
    return node;
}

void Memtable::put(const std::string& key, const std::string& value) {
    Node* existing = findNode(key);
    if (existing) {
        existing->value = value;
        existing->type = EntryType::Live;
        return;
    }

    insertNode(key, value, EntryType::Live);
    ++size_;
}

std::optional<std::string> Memtable::get(const std::string& key) const {
    Node* node = findNode(key);
    if (!node || node->type == EntryType::Tombstone) {
        return std::nullopt;
    }
    return node->value;
}

bool Memtable::contains(const std::string& key) const {
    return findNode(key) != nullptr;
}

void Memtable::remove(const std::string& key) {
    Node* existing = findNode(key);
    if (existing) {
        existing->type = EntryType::Tombstone;
        existing->value.clear();
        return;
    }

    insertNode(key, "", EntryType::Tombstone);
    ++size_;
}

void Memtable::inorderVisit(
    Node* node,
    const std::function<void(const MemtableEntry&)>& fn) const {
    if (!node) {
        return;
    }
    inorderVisit(node->left, fn);
    fn(MemtableEntry{node->key, node->value, node->type});
    inorderVisit(node->right, fn);
}

void Memtable::forEach(
    const std::function<void(const MemtableEntry&)>& fn) const {
    inorderVisit(root_, fn);
}

void Memtable::destroyTree(Node* node) {
    if (!node) {
        return;
    }
    destroyTree(node->left);
    destroyTree(node->right);
    delete node;
}

Memtable::Memtable(Memtable&& other) noexcept
    : root_(other.root_), size_(other.size_) {
    other.root_ = nullptr;
    other.size_ = 0;
}

Memtable& Memtable::operator=(Memtable&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    destroyTree(root_);
    root_ = other.root_;
    size_ = other.size_;
    other.root_ = nullptr;
    other.size_ = 0;
    return *this;
}