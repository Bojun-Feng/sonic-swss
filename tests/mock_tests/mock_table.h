#pragma once

#include "table.h"
#include "rediscommand.h"

#include <cstddef>
#include <deque>
#include <string>

// Use this field in the mock test to simulate an exception during hget.
#define HGET_THROW_EXCEPTION_FIELD_NAME "hget_throw_exception"

namespace testing_db
{
    void reset();

    // Opt-in staging separates producer publication from materialized consumption.
    void enableProducerStaging(bool enabled);
    bool producerStagingEnabled();
    size_t stagedProducerUpdates(int dbId, const std::string &tableName);
    std::deque<swss::KeyOpFieldsValuesTuple> popProducerStaging(int dbId, const std::string &tableName);
    void discardProducerStaging();
}
