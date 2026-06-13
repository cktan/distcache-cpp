#pragma once
#include <cstddef>
#include <string>

// Encode a DATA frame: "DATA:<n>\n<bytes>FIN:0\n"
std::string wire_data(const char *buf, size_t n);

// Encode an INFO frame: "INFO:<n>\n<json>\nFIN:0\n"
std::string wire_info(const std::string &json);

// Encode a REDIRECT frame: "REDIRECT:<node>\nFIN:0\n"
std::string wire_redirect(const std::string &node);

// Encode an ERR frame: "ERR:<msg>\nFIN:0\n"
std::string wire_err(const std::string &msg);

// Encode a bare FIN: "FIN:0\n"
std::string wire_fin();

// Return true iff the string ends with "FIN:0\n"
bool wire_check_fin(const std::string &s);
