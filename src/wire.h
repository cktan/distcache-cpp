#pragma once
#include "stream.h"
#include <cstddef>
#include <string>

// Encode an INFO frame: "INFO:<n>\n<json>\nFIN:0\n"
std::string wire_info(const std::string &json);

// Encode a REDIRECT frame: "REDIRECT:<n>\n\"host:port/first_id\"FIN:0\n"
std::string wire_redirect(const std::string &node);

// Encode an ERR and a FIN frame: "ERR:<msg>\nFIN:0\n"
std::string wire_err(const std::string &msg);

// Encode a bare FIN: "FIN:0\n"
std::string wire_fin();

// Return true iff the string ends with "FIN:0\n"
bool wire_check_fin(const std::string &s);
