#pragma once

#include <string>
#include <string_view>

namespace remoe {

struct HttpResponse {
    unsigned long status = 0;
    std::string body;
};

HttpResponse http_request(std::string_view url, std::wstring_view method,
                          std::string_view json_body = {},
                          std::string_view bearer_token = {});

std::string normalize_server_origin(std::string value);

}  // namespace remoe
