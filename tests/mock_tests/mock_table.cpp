#include "table.h"
#include "schema.h"
#include "producerstatetable.h"
#include "producertable.h"
#include "mock_table.h"
#include <deque>
#include <set>
#include <memory>

using TableDataT = std::map<std::string, std::vector<swss::FieldValueTuple>>;
using TablesT = std::map<std::string, TableDataT>;

namespace swss
{
    void merge_values(std::vector<FieldValueTuple> &existing_values, const std::vector<FieldValueTuple> &values);
}

namespace testing_db
{

    TableDataT gTableData;
    TablesT gTables;
    std::map<int, TablesT> gDB;

    struct StagedUpdate
    {
        std::vector<swss::FieldValueTuple> values;
        bool erase = false;
    };
    using StagedTableT = std::map<std::string, StagedUpdate>;

    static bool gProducerStaging = false;
    static std::map<int, std::map<std::string, StagedTableT>> gStaged;
    static std::map<int, std::map<std::string, std::vector<std::string>>> gStagedKeys;

    void reset()
    {
        gDB.clear();
        discardProducerStaging();
        gProducerStaging = false;
    }

    void enableProducerStaging(bool enabled)
    {
        gProducerStaging = enabled;
    }

    bool producerStagingEnabled()
    {
        return gProducerStaging;
    }

    void discardProducerStaging()
    {
        gStaged.clear();
        gStagedKeys.clear();
    }

    size_t stagedProducerUpdates(int dbId, const std::string &tableName)
    {
        return gStagedKeys[dbId][tableName].size();
    }

    void stageProducerUpdate(int dbId, const std::string &tableName, const std::string &key,
               const std::vector<swss::FieldValueTuple> &values, bool erase)
    {
        auto &staged = gStaged[dbId][tableName];
        auto iter = staged.find(key);
        if (iter == staged.end())
        {
            gStagedKeys[dbId][tableName].push_back(key);
            staged[key] = StagedUpdate{values, erase};
            return;
        }
        if (erase)
        {
            iter->second = StagedUpdate{{}, true};
            return;
        }
        swss::merge_values(iter->second.values, values);
    }

    std::deque<swss::KeyOpFieldsValuesTuple> popProducerStaging(int dbId, const std::string &tableName)
    {
        std::deque<swss::KeyOpFieldsValuesTuple> popped;
        auto &keys = gStagedKeys[dbId][tableName];
        auto &staged = gStaged[dbId][tableName];
        auto &table = gDB[dbId][tableName];
        for (const auto &key : keys)
        {
            auto iter = staged.find(key);
            if (iter == staged.end())
            {
                continue;
            }
            // Match consumer pop ordering: delete before merging staged fields.
            if (iter->second.erase)
            {
                table.erase(key);
            }
            if (iter->second.values.empty())
            {
                popped.push_back(swss::KeyOpFieldsValuesTuple(key, DEL_COMMAND, {}));
                continue;
            }
            auto existing = table.find(key);
            if (existing == table.end())
            {
                table[key] = iter->second.values;
            }
            else
            {
                swss::merge_values(existing->second, iter->second.values);
            }
            popped.push_back(swss::KeyOpFieldsValuesTuple(key, SET_COMMAND, iter->second.values));
        }
        keys.clear();
        staged.clear();
        return popped;
    }
}

namespace swss
{

    using namespace testing_db;

    void merge_values(std::vector<FieldValueTuple> &existing_values, const std::vector<FieldValueTuple> &values)
    {
        std::vector<FieldValueTuple> new_values(values);
        std::set<std::string> field_set;
        for (auto &value : values)
        {
            field_set.insert(fvField(value));
        }
        for (auto &value : existing_values)
        {
            auto &field = fvField(value);
            if (field_set.find(field) != field_set.end())
            {
                continue;
            }
            new_values.push_back(value);
        }
        existing_values.swap(new_values);
    }

    bool _hget(int dbId, const std::string &tableName, const std::string &key, const std::string &field, std::string &value)
    {
        auto table = gDB[dbId][tableName];
        if (table.find(key) == table.end())
        {
            return false;
        }

        for (const auto &it : table[key])
        {
            if (it.first == field)
            {
                value = it.second;
                return true;
            }
        }

        return false;
    }

    bool Table::get(const std::string &key, std::vector<FieldValueTuple> &ovalues)
    {
        auto table = gDB[m_pipe->getDbId()][getTableName()];
        if (table.find(key) == table.end())
        {
            return false;
        }

        ovalues = table[key];
        return true;
    }

    bool Table::hget(const std::string &key, const std::string &field, std::string &value)
    {
        return _hget(m_pipe->getDbId(), getTableName(), key, field, value);
    }

    void Table::set(const std::string &key,
                    const std::vector<FieldValueTuple> &values,
                    const std::string &op,
                    const std::string &prefix)
    {
        auto &table = gDB[m_pipe->getDbId()][getTableName()];
        auto iter = table.find(key);
        if (iter == table.end())
        {
            table[key] = values;
        }
        else
        {
            merge_values(iter->second, values);
        }
    }

    void Table::hset(const std::string &key, const std::string &field, const std::string &value,
                const std::string& op, const std::string& prefix)
    {
        FieldValueTuple fvp(field, value);
        std::vector<FieldValueTuple> attrs = { fvp };

        Table::set(key, attrs, op, prefix);
    }

    void Table::getKeys(std::vector<std::string> &keys)
    {
        keys.clear();
        auto table = gDB[m_pipe->getDbId()][getTableName()];
        for (const auto &it : table)
        {
            keys.push_back(it.first);
        }
    }

    void Table::del(const std::string &key, const std::string& /* op */, const std::string& /*prefix*/)
    {
        auto table = gDB[m_pipe->getDbId()].find(getTableName());
        if (table != gDB[m_pipe->getDbId()].end()){
            table->second.erase(key);
        }
    }

    void Table::hdel(const std::string &key, const std::string &field, const std::string &op, const std::string &prefix)
    {
        auto &table = gDB[m_pipe->getDbId()][getTableName()];
        auto key_iter = table.find(key);
        if (key_iter == table.end())
        {
            return;
        }

        auto &attrs = key_iter->second;
        std::vector<FieldValueTuple> new_attrs;
        for (const auto &attr : attrs)
        {
            if (attr.first != field)
            {
                new_attrs.push_back(attr);
            }
        }

        if (new_attrs.empty())
        {
            table.erase(key);
        }
        else
        {
            table[key] = new_attrs;
        }
    }
    
    void ProducerStateTable::set(const std::string &key,
                                 const std::vector<FieldValueTuple> &values,
                                 const std::string &op,
                                 const std::string &prefix)
    {
        if (producerStagingEnabled())
        {
            stageProducerUpdate(m_pipe->getDbId(), getTableName(), key, values, false);
            return;
        }
        auto &table = gDB[m_pipe->getDbId()][getTableName()];
        auto iter = table.find(key);
        if (iter == table.end())
        {
            table[key] = values;
        }
        else
        {
            merge_values(iter->second, values);
        }
    }

    void ProducerStateTable::del(const std::string &key,
                                 const std::string &op,
                                 const std::string &prefix)
    {
        if (producerStagingEnabled())
        {
            stageProducerUpdate(m_pipe->getDbId(), getTableName(), key, {}, true);
            return;
        }
        auto &table = gDB[m_pipe->getDbId()][getTableName()];
        table.erase(key);
    }

    void ProducerStateTable::set(const std::vector<KeyOpFieldsValuesTuple>& values)
    {
        for (const auto& kfv : values)
        {
            const std::string& key = kfvKey(kfv);
            const std::string& op = kfvOp(kfv);
            const std::vector<FieldValueTuple>& fvs = kfvFieldsValues(kfv);

            if (op == SET_COMMAND)
            {
                set(key, fvs);
            }
            else if (op == DEL_COMMAND)
            {
                del(key);
            }
        }
    }

    void ProducerStateTable::del(const std::vector<std::string>& keys)
    {
        for (const auto& key : keys)
        {
            del(key);
        }
    }

    std::shared_ptr<std::string> DBConnector::hget(const std::string &key, const std::string &field)
    {
        std::string value;

        if (field == HGET_THROW_EXCEPTION_FIELD_NAME)
        {
            throw std::runtime_error("HGET failed, unexpected reply type, memory exception");
        }

        if (_hget(getDbId(), key, "", field, value))
        {
            std::shared_ptr<std::string> ptr(new std::string(value));
            return ptr;
        }
        else
        {
            return std::shared_ptr<std::string>(NULL);
        }
    }

    int64_t DBConnector::hdel(const std::string &key, const std::string &field)
    {
        auto &table = gDB[getDbId()][key];
        auto key_iter = table.find("");
        if (key_iter == table.end())
        {
            return 0;
        }

        int removed = 0;
        auto attrs = key_iter->second;
        std::vector<FieldValueTuple> new_attrs;
        for (auto attr_iter : attrs)
        {
            if (attr_iter.first == field)
            {
                removed += 1;
                continue;
            }

            new_attrs.push_back(attr_iter);
        }

        table[""] = new_attrs;

        return removed;
    }

    void DBConnector::hset(const std::string &key, const std::string &field, const std::string &value)
    {
        FieldValueTuple fvp(field, value);
        std::vector<FieldValueTuple> attrs = { fvp };

        auto &table = gDB[getDbId()][key];
        auto iter = table.find("");
        if (iter == table.end())
        {
            table[""] = attrs;
        }
        else
        {
            merge_values(iter->second, attrs);
        }
    }

    void ProducerTable::set(const std::string &key,
                            const std::vector<FieldValueTuple> &values,
                            const std::string &op,
                            const std::string &prefix)
    {
        auto &table = gDB[m_pipe->getDbId()][getTableName()];
        auto iter = table.find(key);
        if (iter == table.end())
        {
            table[key] = values;
        }
        else
        {
            merge_values(iter->second, values);
        }
    }

    void ProducerTable::del(const std::string &key,
                            const std::string &op,
                            const std::string &prefix)
    {
        auto &table = gDB[m_pipe->getDbId()][getTableName()];
        table.erase(key);
    }
}
