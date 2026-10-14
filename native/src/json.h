/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_JSON_H
#define SD_JSON_H

#include <memory>
#include <string>
#include <string_view>
#include <vector>

enum JType { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

struct JVal {
    JType type = J_NULL;
    double num = 0;
    bool b = false;
    std::string str;
    std::vector<std::unique_ptr<JVal>> items;
    std::vector<std::string> keys;
};

std::unique_ptr<JVal> json_parse(std::string_view text, std::string *err = nullptr);
const JVal *json_get(const JVal *obj, std::string_view key);
const char *json_str(const JVal *v, const char *def);
double json_num(const JVal *v, double def);
void json_append_string(std::string &out, std::string_view s);

#endif
