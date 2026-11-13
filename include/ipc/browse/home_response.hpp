#pragma once

#include "../../models/api_result.hpp"
#include "../response.hpp"

#include <unordered_set>
#include <unordered_map>

namespace ipc {

class HomeResponse : public Response {
public:
    std::unordered_map<std::string, std::vector<music::ApiResult>> results;

    // Categories in the order YouTube Music sent them; results alone loses it.
    std::vector<std::string> order;

    explicit HomeResponse() : Response() {}

    /* addItem
    - Every shelf YouTube Music sends is kept (Your daily discover, Mixed for
      you, New releases...), not a fixed list, so the page follows the account
    */
    void addItem(const std::string& category, const nlohmann::json& j) {
        if (category.empty()) return;

        auto [it, inserted] = results.try_emplace(category);
        if (inserted) order.push_back(category);

        it->second.emplace_back(music::ApiResult::from_json(j));
    }
};

}
