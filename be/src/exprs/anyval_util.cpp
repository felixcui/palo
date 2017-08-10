// Modifications copyright (C) 2017, Baidu.com, Inc.
// Copyright 2017 The Apache Software Foundation

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "exprs/anyval_util.h"

namespace palo {
using palo_udf::BooleanVal;
using palo_udf::TinyIntVal;
using palo_udf::SmallIntVal;
using palo_udf::IntVal;
using palo_udf::BigIntVal;
using palo_udf::LargeIntVal;
using palo_udf::FloatVal;
using palo_udf::DoubleVal;
using palo_udf::DecimalVal;
using palo_udf::DateTimeVal;
using palo_udf::StringVal;
using palo_udf::AnyVal;

AnyVal* create_any_val(ObjectPool* pool, const TypeDescriptor& type) {
    switch (type.type) {
    case TYPE_NULL:
        return pool->add(new AnyVal);

    case TYPE_BOOLEAN:
        return pool->add(new BooleanVal);

    case TYPE_TINYINT:
        return pool->add(new TinyIntVal);

    case TYPE_SMALLINT:
        return pool->add(new SmallIntVal);

    case TYPE_INT:
        return pool->add(new IntVal);

    case TYPE_BIGINT:
        return pool->add(new BigIntVal);

    case TYPE_LARGEINT:
        return pool->add(new LargeIntVal);

    case TYPE_FLOAT:
        return pool->add(new FloatVal);

    case TYPE_DOUBLE:
        return pool->add(new DoubleVal);

    case TYPE_HLL:
    case TYPE_VARCHAR:
        return pool->add(new StringVal);

    case TYPE_DECIMAL:
        return pool->add(new DecimalVal);

    case TYPE_DATE:
        return pool->add(new DateTimeVal);

    case TYPE_DATETIME:
        return pool->add(new DateTimeVal);
default:
        DCHECK(false) << "Unsupported type: " << type.type;
        return NULL;
    }
}

FunctionContext::TypeDesc AnyValUtil::column_type_to_type_desc(const TypeDescriptor& type) {
    FunctionContext::TypeDesc out;
    switch (type.type) {
    case TYPE_BOOLEAN:
        out.type = FunctionContext::TYPE_BOOLEAN;
        break;
    case TYPE_TINYINT:
        out.type = FunctionContext::TYPE_TINYINT;
        break;
    case TYPE_SMALLINT:
        out.type = FunctionContext::TYPE_SMALLINT;
        break;
    case TYPE_INT:
        out.type = FunctionContext::TYPE_INT;
        break;
    case TYPE_BIGINT:
        out.type = FunctionContext::TYPE_BIGINT;
        break;
    case TYPE_LARGEINT:
        out.type = FunctionContext::TYPE_LARGEINT;
        break;
    case TYPE_FLOAT:
        out.type = FunctionContext::TYPE_FLOAT;
        break;
    case TYPE_DOUBLE:
        out.type = FunctionContext::TYPE_DOUBLE;
        break;
    case TYPE_DATE:
        out.type = FunctionContext::TYPE_DATE;
        break;
    case TYPE_DATETIME:
        out.type = FunctionContext::TYPE_DATETIME;
        break;
    case TYPE_VARCHAR:
        out.type = FunctionContext::TYPE_VARCHAR;
        out.len = type.len;
        break;
    case TYPE_HLL:
        out.type = FunctionContext::TYPE_HLL;
        out.len = type.len;
        break; 
    case TYPE_CHAR:
        out.type = FunctionContext::TYPE_FIXED_BUFFER;
        out.len = type.len;
        break;
    case TYPE_DECIMAL:
        out.type = FunctionContext::TYPE_DECIMAL;
        // out.precision = type.precision;
        // out.scale = type.scale;
        break;
    case TYPE_NULL:
        out.type = FunctionContext::TYPE_NULL;
        break;
    default:
        DCHECK(false) << "Unknown type: " << type;
    }
    return out;
}

}