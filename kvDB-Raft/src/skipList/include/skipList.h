#pragma once

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include <mutex>

#define STORE_FILE "store/dumpFile"

static std::string delimiter = ":";

template <typename K, typename V>
class Node
{
public:
    Node() {}
    Node(K k, V v, int);
    ~Node();
    K get_key() const;
    V get_value() const;
    void set_value(V);

    Node<K, V> **forward; // 指向当前节点在该层的下一个节点 比如A.forward[0] -> B， 就是A在第0层上的下一个节点是B
    int node_level;

private:
    K key;
    V value;
};

template <typename K, typename V>
Node<K, V>::Node(const K k, const V v, int level)
    : key(k), value(v), node_level(level)
{
    // level + 1, because array index is from 0 - level
    this->forward = new Node<K, V> *[level + 1];

    // Fill forward array with 0(NULL)
    memset(this->forward, 0, sizeof(Node<K, V> *) * (level + 1));
}

template <typename K, typename V>
Node<K, V>::~Node()
{
    delete[] forward;
}

template <typename K, typename V>
K Node<K, V>::get_key() const
{
    return key;
}

template <typename K, typename V>
V Node<K, V>::get_value() const
{
    return value;
}

template <typename K, typename V>
void Node<K, V>::set_value(V value)
{
    this->value = value;
}

/**
 * 持久化跳表数据
 * 用来把跳表中的所有 key 和 value 拷贝出来，方便保存到磁盘文件 / 网络传输 / 快照保存等用途
 */
template <typename K, typename V>
class SkipListDump
{
public:
    friend class boost::serialization::access;

    template <class Archive>
    void serialize(Archive &ar, const unsigned int version)
    {
        ar & keyDumpVt_;
        ar & valueDumpVt_;
    }
    std::vector<K> keyDumpVt_;
    std::vector<V> valueDumpVt_;

public:
    void insert(const Node<K, V> &node);
};

template <typename K, typename V>
class SkipList
{
public:
    SkipList(int);
    ~SkipList();
    int get_random_level();
    Node<K, V> *create_node(K, V, int);
    // 插入一个键值对。如果 key 存在则返回 1，否则插入并返回 0
    int insert_element(K, V);
    void display_list();
    bool search_element(K, V &value);
    void delete_element(K);
    void insert_set_element(K &, V &);
    std::string dump_file();
    void load_file(const std::string &dumpStr);
    // 递归删除节点
    void clear(Node<K, V> *);
    int size();

private:
    void get_key_value_from_string(const std::string &str, std::string *key, std::string *value);
    bool is_valid_string(const std::string &str);

private:
    int _max_level;       // 最大支持层级
    int _skip_list_level; // 当前跳表层级（实际层高）

    Node<K, V> *_header; // 跳表中最顶层、最左边的节点，不存储有效 key-value 数据，只是作为所有层级的起始点。于是第i层的第一个节点是_header->froward[i]

    std::ofstream _file_writer;
    std::ifstream _file_reader;

    int _element_count; // 节点数
    std::mutex _mtx;    // 线程安全保护
};

template <typename K, typename V>
Node<K, V> *SkipList<K, V>::create_node(const K k, const V v, int level)
{
    Node<K, V> *n = new Node<K, V>(k, v, level);
    return n;
}

template <typename K, typename V>
int SkipList<K, V>::insert_element(const K key, const V value)
{
    _mtx.lock();
    Node<K, V> *current = this->_header;

    Node<K, V> *update[_max_level + 1];
    memset(update, 0, sizeof(Node<K, V> *) * (_max_level + 1));

    for (int i = _skip_list_level; i >= 0; i--)
    {
        while (current->forward[i] != nullptr && current->forward[i]->get_key() < key)
            current = current->forward[i];
        update[i] = current; // update[i]记录插入节点在每一层的前驱节点
    }

    current = current->forward[0]; // 进入第0层，将current变为插入位置的下一个节点

    // 如果当前节点不为空，并且当前节点的key等于要插入的key，说明该key已经存在
    if (current != nullptr && current->get_key() == key)
    {
        std::cout << "key: " << key << ", exists" << std::endl;
        _mtx.unlock();
        return 1;
    }

    // 如果当前节点为空，或者当前节点的key不等于要插入的key，说明该key不存在
    if (current == nullptr || current->get_key() != key)
    {
        int random_level = get_random_level();

        // 如果随机层数大于当前跳表的层数，则需要更新跳表的层数
        if (random_level > _skip_list_level)
        {
            for (int i = _skip_list_level + 1; i <= random_level; i++)
            {
                update[i] = _header;
            }
            _skip_list_level = random_level;
        }

        Node<K, V> *inserted_node = create_node(key, value, random_level);

        for (int i = 0; i <= random_level; i++)
        {
            inserted_node->forward[i] = update[i]->forward[i];
            update[i]->forward[i] = inserted_node;
        }

        std::cout << "Successfully inserted key:" << key << ", value:" << value << std::endl;
        _element_count++;
    }

    _mtx.unlock();
    return 0;
}

template <typename K, typename V>
void SkipList<K, V>::display_list()
{
    std::cout << "\n*****Skip List*****" << std::endl;
    for (int i = 0; i <= _skip_list_level; i++)
    {
        Node<K, V> *node = _header->forward[i];
        std::cout << "Level " << i << ": ";
        while (node != nullptr)
        {
            std::cout << node->get_key() << ":" << node->get_value() << ";";
            node = node->forward[i];
        }
        std::cout << std::endl;
    }
}

// 负责将数据序列化成字符串
template <typename K, typename V>
std::string SkipList<K, V>::dump_file()
{
    // 第 0 层包含了所有元素
    Node<K, V> *node = _header->forward[0];
    SkipListDump<K, V> dumper;
    while (node != nullptr)
    {
        dumper.insert(*node);
        node = node->forward[0];
    }

    std::stringstream ss;
    boost::archive::text_oarchive oa(ss);
    oa << dumper;
    return ss.str();
}

// 负责从字符串中恢复跳表数据
template <typename K, typename V>
void SkipList<K, V>::load_file(const std::string &dumpStr)
{
    if (dumpStr.empty())
    {
        return;
    }

    std::stringstream ss(dumpStr);
    boost::archive::text_iarchive ia(ss);
    SkipListDump<K, V> dumper;
    ia >> dumper; // 此时dumper.keyDumpVt_ 和 dumper.valueDumpVt_ 中应该就装好了所有键值对。

    for (size_t i = 0; i < dumper.keyDumpVt_.size(); ++i)
    {
        insert_element(dumper.keyDumpVt_[i], dumper.valueDumpVt_[i]);
    }
}

template <typename K, typename V>
int SkipList<K, V>::size()
{
    return _element_count;
}

template <typename K, typename V>
void SkipList<K, V>::get_key_value_from_string(const std::string &str, std::string *key, std::string *value)
{
    if (!is_valid_string(str))
        return;

    *key = str.substr(0, str.find(delimiter));
    *value = str.substr(str.find(delimiter) + 1, str.length());
}

template <typename K, typename V>
bool SkipList<K, V>::is_valid_string(const std::string &str)
{
    if (str.empty())
        return false;
    if (str.find(delimiter) == std::string::npos)
        return false;
    return true;
}

template <typename K, typename V>
void SkipList<K, V>::delete_element(K key)
{
    _mtx.lock();
    Node<K, V> *current = this->_header;
    Node<K, V> *update[_max_level + 1];
    memset(update, 0, sizeof(Node<K, V> *) * (_max_level + 1));

    // start from highest level of skip list
    for (int i = _skip_list_level; i >= 0; i--)
    {
        // 每一层走完后，current 已经指向“当前层上最靠近 key 的节点”，这个位置也就是下一层的起点！
        while (current->forward[i] != nullptr && current->forward[i]->get_key() < key)
        {
            current = current->forward[i];
        }
        update[i] = current;
    }

    current = current->forward[0]; // 要删除的节点
    if (current != nullptr && current->get_key() == key)
    {
        for (int i = 0; i <= _skip_list_level; i++)
        {
            if (update[i]->forward[i] != current)
                break;
            update[i]->forward[i] = current->forward[i];
        }

        while (_skip_list_level > 0 && _header->forward[_skip_list_level] == nullptr)
        {
            _skip_list_level--;
        }

        std::cout << "Successfully deleted key " << key << std::endl;
        delete current;
        _element_count--;
    }

    _mtx.unlock();
    return;
}

/**
 * \brief 作用与insert_element相同类似，
 * insert_element是插入新元素，
 * insert_set_element是插入元素，如果元素存在则改变其值
 */
template <typename K, typename V>
void SkipList<K, V>::insert_set_element(K &key, V &value)
{
    V oldValue;
    if (search_element(key, oldValue))
        delete_element(key);
    insert_element(key, value);
}

template <typename K, typename V>
bool SkipList<K, V>::search_element(K key, V &value)
{
    std::cout << "search_element-----------------" << std::endl;
    Node<K, V> *current = _header;

    for (int i = _skip_list_level; i >= 0; i--)
    {
        while (current->forward[i] && current->forward[i]->get_key() < key)
            current = current->forward[i];
    }
    current = current->forward[0];
    if (current != nullptr && current->get_key() == key)
    {
        value = current->get_value();
        std::cout << "Found key: " << key << ", value: " << current->get_value() << std::endl;
        return true;
    }

    std::cout << "Not Found Key:" << key << std::endl;
    return false;
}

template <typename K, typename V>
void SkipListDump<K, V>::insert(const Node<K, V> &node)
{
    keyDumpVt_.emplace_back(node.get_key());
    valueDumpVt_.emplace_back(node.get_value());
}

template <typename K, typename V>
SkipList<K, V>::SkipList(int max_level)
{
    this->_max_level = max_level;
    this->_skip_list_level = 0;
    this->_element_count = 0;

    K k;
    V v;
    this->_header = new Node<K, V>(k, v, _max_level);
}

template <typename K, typename V>
SkipList<K, V>::~SkipList()
{
    if (_file_writer.is_open())
        _file_writer.close();
    if (_file_reader.is_open())
    {
        _file_reader.close();
    }

    if (_header->forward[0] != nullptr)
        clear(_header->forward[0]);
    delete _header;
}

template <typename K, typename V>
void SkipList<K, V>::clear(Node<K, V> *cur)
{
    if (cur->forward[0] != nullptr)
        clear(cur->forward[0]);
    delete cur;
}

template <typename K, typename V>
int SkipList<K, V>::get_random_level()
{
    int k = 1;
    while (rand() % 2)
        k++;
    k = (k < _max_level) ? k : _max_level;
    return k;
}