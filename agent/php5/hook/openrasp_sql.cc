/*
 * Copyright 2017-2019 Baidu Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "openrasp_hook.h"
#include "openrasp_ini.h"
#include "openrasp_v8.h"
#include "openrasp_shared_alloc.h"
#include <string>
#include <map>

extern "C"
{
#include "ext/pdo/php_pdo_driver.h"
#include "zend_ini.h"
}

/**
 * sql connection alarm
 */
static void connection_via_default_username_policy(sql_connection_entry *sql_connection_p TSRMLS_DC)
{
    zval *policy_array = nullptr;
    MAKE_STD_ZVAL(policy_array);
    array_init(policy_array);
    add_assoc_string(policy_array, "message", (char *)sql_connection_p->build_policy_msg().c_str(), 1);
    add_assoc_long(policy_array, "policy_id", 3006);
    zval *connection_params = nullptr;
    MAKE_STD_ZVAL(connection_params);
    array_init(connection_params);
    add_assoc_string(connection_params, "server", (char *)sql_connection_p->get_server().c_str(), 1);
    add_assoc_string(connection_params, "hostname", (char *)sql_connection_p->get_host().c_str(), 1);
    add_assoc_string(connection_params, "username", (char *)sql_connection_p->get_username().c_str(), 1);
    add_assoc_string(connection_params, "socket", (char *)sql_connection_p->get_socket().c_str(), 1);
    add_assoc_string(connection_params, "connectionString", (char *)sql_connection_p->get_connection_string().c_str(), 1);
    add_assoc_long(connection_params, "port", sql_connection_p->get_port());
    add_assoc_zval(policy_array, "policy_params", connection_params);
    LOG_G(policy_logger).log(LEVEL_INFO, policy_array TSRMLS_CC);
    zval_ptr_dtor(&policy_array);
}

void slow_query_alarm(int rows TSRMLS_DC)
{
    zval *attack_params = nullptr;
    MAKE_STD_ZVAL(attack_params);
    array_init(attack_params);
    add_assoc_long(attack_params, "query_count", rows);
    zval *plugin_message = nullptr;
    MAKE_STD_ZVAL(plugin_message);
    char *message_str = nullptr;
    spprintf(&message_str, 0, _("SQL slow query detected: selected %d rows, exceeding %ld"),
             rows, OPENRASP_CONFIG(sql.slowquery.min_rows));
    ZVAL_STRING(plugin_message, message_str, 1);
    efree(message_str);
    openrasp_buildin_php_risk_handle(AC_LOG, SQL_SLOW_QUERY, 100, attack_params, plugin_message TSRMLS_CC);
}

zend_bool check_database_connection_username(INTERNAL_FUNCTION_PARAMETERS, init_connection_t connection_init_func, int enforce_policy)
{
    static const std::multimap<std::string, std::string> database_username_blacklists = {
        {"mysql", "root"},
        {"mssql", "sa"},
        {"pgsql", "postgres"},
        {"oci", "dbsnmp"},
        {"oci", "sysman"},
        {"oci", "system"},
        {"oci", "sys"}};

    sql_connection_entry conn_entry;
    bool high_privileged_found = false;
    zend_bool need_block = 0;

    connection_init_func(INTERNAL_FUNCTION_PARAM_PASSTHRU, &conn_entry);

    if (!conn_entry.get_server().empty() &&
        !conn_entry.get_username().empty() &&
        !conn_entry.get_host().empty())
    {
        auto pos = database_username_blacklists.equal_range(conn_entry.get_server());
        while (pos.first != pos.second)
        {
            if (conn_entry.get_username() == pos.first->second)
            {
                high_privileged_found = true;
                break;
            }
            pos.first++;
        }
        if (high_privileged_found)
        {
            if (enforce_policy)
            {
                connection_via_default_username_policy(&conn_entry TSRMLS_CC);
                need_block = 1;
            }
            else
            {
                if (check_sapi_need_alloc_shm())
                {
                    ulong connection_hash = conn_entry.build_hash_code();
                    openrasp_shared_alloc_lock(TSRMLS_C);
                    if (!openrasp_shared_hash_exist(connection_hash, LOG_G(alarm_logger).get_formatted_date_suffix()))
                    {
                        connection_via_default_username_policy(&conn_entry TSRMLS_CC);
                    }
                    openrasp_shared_alloc_unlock(TSRMLS_C);
                }
            }
        }
    }
    return need_block;
}

void plugin_sql_check(char *query, int query_len, char *server TSRMLS_DC)
{
    openrasp::Isolate *isolate = OPENRASP_V8_G(isolate);
    if (isolate)
    {
        std::string cache_key = std::string(get_check_type_name(SQL)).append(query, query_len);
        if (OPENRASP_HOOK_G(lru)->contains(cache_key))
        {
            return;
        }
        bool is_block = false;
        {
            v8::HandleScope handle_scope(isolate);
            auto params = v8::Object::New(isolate);
            params->Set(openrasp::NewV8String(isolate, "query"), openrasp::NewV8String(isolate, query, query_len));
            params->Set(openrasp::NewV8String(isolate, "server"), openrasp::NewV8String(isolate, server));
            is_block = isolate->Check(openrasp::NewV8String(isolate, get_check_type_name(SQL)), params, OPENRASP_CONFIG(plugin.timeout.millis));
        }
        if (is_block)
        {
            handle_block(TSRMLS_C);
        }
        OPENRASP_HOOK_G(lru)->set(cache_key, true);
    }
}

long fetch_rows_via_user_function(const char *f_name_str, zend_uint param_count, zval *params[] TSRMLS_DC)
{
    zval function_name, retval;
    INIT_ZVAL(function_name);
    ZVAL_STRING(&function_name, f_name_str, 0);
    if (call_user_function(EG(function_table), nullptr, &function_name, &retval, param_count, params TSRMLS_CC) == SUCCESS && Z_TYPE(retval) == IS_LONG)
    {
        return Z_LVAL(retval);
    }
    return 0;
}
