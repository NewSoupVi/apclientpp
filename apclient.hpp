/* Copyright (c) 2022 black-sliver, FelicitusNeko

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#ifndef _APCLIENT_HPP
#define _APCLIENT_HPP


#define APCLIENTPP_VERSION_INITIALIZER {0, 6, 0}


#if defined _WSWRAP_HPP && !defined WSWRAP_SEND_EXCEPTIONS
#warning "Can't set exception behavior. wswrap already included"
#elif !defined WSWRAP_SEND_EXCEPTIONS
#define WSWRAP_SEND_EXCEPTIONS // backwards compatibility for at least 1 version
#endif


//#define APCLIENT_DEBUG // to get debug output
//#define AP_NO_DEFAULT_DATA_PACKAGE_STORE // to disable auto-construction of data package store
//#define AP_NO_SCHEMA // to disable schema checking
//#define AP_PREFER_UNENCRYPTED // try unencrypted connection first, then encrypted


#include <wswrap.hpp>
#include <list>
#include <map>
#include <set>
#include <string>
#include <tuple>
#if defined(_MSC_VER) && _MSC_VER < 1910 // older msvc doesn't like the has_include
#define NO_OPTIONAL
#else
#if (defined __cplusplus && __cplusplus >= 201703L) || (defined __has_include && __has_include(<optional>))
#include <optional>
#elif defined __has_include && __has_include(<experimental/optional>)
#include <experimental/optional>
#else
#define NO_OPTIONAL
#endif
#endif

#include <nlohmann/json.hpp>
#ifndef AP_NO_SCHEMA
#include <valijson/adapters/nlohmann_json_adapter.hpp>
#include <valijson/schema.hpp>
#include <valijson/schema_parser.hpp>
#include <valijson/validator.hpp>
#endif
#include <chrono>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <limits>


#ifndef WSWRAP_VERSION
#define WSWRAP_VERSION 10000 // 1.0 did not have this define
#endif

#ifndef __EMSCRIPTEN__
#if WSWRAP_VERSION < 10300
#warning "Please update wswrap to enable compression! Archipelago will require compression in the future."
#elif defined WSWRAP_NO_COMPRESSION
#warning "Don't disable it in prod! Archipelago will require compression in the future."
#endif
#endif


/**
 * Abstract data package storage handler.
 *
 * Inherit, instantiate and pass to APClient's constructor to handle data package caching.
 * A default implementation is `DefaultDataPackageStore` in `defaultdatapackagestore.hpp`.
 */
class APDataPackageStore {
protected:
    typedef nlohmann::json json;

    APDataPackageStore() {}

public:
    virtual ~APDataPackageStore() {}

    virtual bool load(const std::string& game, const std::string& checksum, json& data) = 0;
    virtual bool save(const std::string& game, const json& data) = 0;
};


#ifndef AP_NO_DEFAULT_DATA_PACKAGE_STORE
#include "defaultdatapackagestore.hpp"
#endif


/**
 * Archipelago Client implementation.
 *
 * Instantiate, hook up callbacks and call `poll()` repeatedly to attach your game to a server.
 */
class APClient {
protected:
    typedef nlohmann::json json;
    typedef wswrap::WS WS;
#ifndef AP_NO_SCHEMA
    typedef valijson::adapters::NlohmannJsonAdapter JsonSchemaAdapter;
#endif

    static int64_t stoi64(const std::string& s) {
        return std::stoll(s);
    }

public:
    static constexpr int64_t INVALID_NAME_ID = std::numeric_limits<int64_t>::min();
#if !defined _MSC_VER || _MSC_VER >= 1911
    static constexpr char DEFAULT_URI[] = "localhost:38281";
#else
    static const char DEFAULT_URI[]; // = "localhost:38281"; // assign this in implementation
#endif

    APClient(const std::string& uuid, const std::string& game, const std::string& uri = "localhost:38281",
             const std::string& certStore="", APDataPackageStore* dataPackageStore = nullptr)
        : _dataPackageStore(dataPackageStore)
    {
        // check if certStore is supported and required
        #if WSWRAP_VERSION < 10100 && !defined __EMSCRIPTEN__
        if (!certStore.empty()) {
            log("Cert store not supported, please update wswrap!\n");
        }
        #elif !defined __EMSCRIPTEN__
        _certStore = certStore;
        #else
        (void)certStore; // avoid warning
        #endif

        // check if wss is requested and supported
        #if WSWRAP_VERSION < 10100 && !defined __EMSCRIPTEN__
        if (uri.rfind("wss://", 0) == 0) {
            auto msg = "No SSL support. Please update wswrap library!";
            log(msg);
            throw std::invalid_argument(msg);
        } else
        #endif

        // fix up URI (add ws:// and default port if none is given)
        if (!uri.empty()) {
            auto p = uri.find("://");
            if (p == uri.npos) {
            #if WSWRAP_VERSION >= 10100 || defined __EMSCRIPTEN__
                _tryWSS = true;
                #ifdef AP_PREFER_UNENCRYPTED
                _uri = "ws://" + uri;
                p = 2;
                #else
                _uri = "wss://" + uri;
                p = 3;
                #endif
            #else // no ssl support -> ws://
                _uri = "ws://" + uri;
                p = 2;
            #endif
            } else {
                _uri = uri;
            }
            auto pColon = _uri.find(":", p + 3); // FIXME: this fails for IPv6 addresses
            auto pSlash = _uri.find("/", p + 3);
            if (pColon == _uri.npos || (pSlash != _uri.npos && pColon > pSlash)) {
                auto tmp = _uri.substr(0, pSlash) + ":38281";
                if (pSlash != _uri.npos) tmp += _uri.substr(pSlash);
                _uri = tmp;
            }
        }

        if (!_dataPackageStore) {
        #ifndef AP_NO_DEFAULT_DATA_PACKAGE_STORE
            _autoDataPackageStore.reset(new DefaultDataPackageStore());
            _dataPackageStore =_autoDataPackageStore.get();
        #else
            const char* msg = "dataPackageStore is required if compiled with AP_NO_DEFAULT_DATA_PACKAGE_STORE";
            fprintf(stderr, "APClient: %s!\n", msg);
            #ifdef __cpp_exceptions
            throw std::runtime_error(msg);
            #endif
        #endif
        }

        _uuid = uuid;
        _game = game;
        _dataPackage = {
            {"version", -1},
            {"games", json(json::value_t::object)},
        };

        #ifndef AP_NO_SCHEMA
        valijson::SchemaParser parser;
        parser.populateSchema(JsonSchemaAdapter(_packetSchemaJson), _packetSchema);
        parser.populateSchema(JsonSchemaAdapter(_retrievedSchemaJson), _commandSchemas["Retrieved"]);
        parser.populateSchema(JsonSchemaAdapter(_setReplySchemaJson), _commandSchemas["SetReply"]);
        #endif

        connect_socket();
    }

    virtual ~APClient()
    {
    }

    enum class State {
        DISCONNECTED,
        SOCKET_CONNECTING,
        SOCKET_CONNECTED,
        ROOM_INFO,
        SLOT_CONNECTED,
    };

    enum class ClientStatus : int {
        UNKNOWN = 0,
        READY = 10,
        PLAYING = 20,
        GOAL = 30,
    };

    enum class RenderFormat {
        TEXT,
        HTML,
        ANSI,
    };

    enum ItemFlags {
        FLAG_NONE = 0,
        FLAG_ADVANCEMENT = 1,
        FLAG_NEVER_EXCLUDE = 2,
        FLAG_TRAP = 4,
    };

    enum HintStatus {
        HINT_UNSPECIFIED = 0,  ///< The receiving player has not specified any status
        HINT_NO_PRIORITY = 10, ///< The receiving player has specified that the item is unneeded
        HINT_AVOID = 20,       ///< The receiving player has specified that the item is detrimental
        HINT_PRIORITY = 30,    ///< The receiving player has specified that the item is needed
        HINT_FOUND = 40,       ///< The location has been collected. Status cannot be changed once found.
    };

    enum class SlotType : int {
        SPECTATOR = 0,
        PLAYER = 1,
        GROUP = 2,
    };

    struct NetworkItem {
        int64_t item;
        int64_t location;
        int player;
        unsigned flags;
        int index = -1; // to sync items, not actually part of NetworkItem
    };

    struct NetworkPlayer {
        int team;
        int slot;
        std::string alias;
        std::string name;

        friend void to_json(nlohmann::json &j, const NetworkPlayer &player)
        {
            j = nlohmann::json{
                {"team", player.team},
                {"slot", player.slot},
                {"alias", player.alias},
                {"name", player.name},
            };
        }
    };

    struct NetworkSlot {
        std::string name;
        std::string game;
        SlotType type;
        std::list<int> members;
    };

    struct TextNode {
        std::string type;
        std::string color;
        std::string text;
        int player = 0;
        unsigned flags = FLAG_NONE;
        unsigned hintStatus = HINT_UNSPECIFIED;

        static TextNode from_json(const json& j)
        {
            TextNode node;
            node.type = j.value("type", "");
            node.color = j.value("color", "");
            node.text = j.value("text", "");
            node.player = j.value("player", 0);
            node.flags = j.value("flags", 0U);
            node.hintStatus = j.value("hint_status", 0U);
            return node;
        }
    };

    /**
     * Parsed arguments of PrintJSON.
     * Pointer arguments are optional (null if missing).
     * You can not store any pointer. You'll have to store a copy of the value.
     */
    struct PrintJSONArgs {
        std::list<TextNode> data;
        std::string type;
        // members below are optional and absent when null
        int* receiving = nullptr;
        NetworkItem* item = nullptr;
        bool* found = nullptr;
        int* team = nullptr;
        int* slot = nullptr;
        std::string* message = nullptr;
        std::list<std::string>* tags = nullptr;
        int* countdown = nullptr;
    };

    struct Version {
        int ma;
        int mi;
        int build;

        friend void to_json(nlohmann::json& j, const Version& ver)
        {
            j = nlohmann::json{
                {"major", ver.ma},
                {"minor", ver.mi},
                {"build", ver.build},
                {"class", "Version"}
            };
        }

        static Version from_json(const nlohmann::json& j)
        {
            if (j.is_null()) {
                return {0, 0, 0};
            }
            return {
                j.value("major", 0),
                j.value("minor", 0),
                j.value("build", 0)
            };
        }

        constexpr bool operator<(const Version& other) const
        {
            return (ma < other.ma) || (ma == other.ma && mi < other.mi) ||
                   (ma == other.ma && mi == other.mi && build < other.build);
        }

        constexpr bool operator>=(const Version& other) const
        {
            return !(*this < other);
        }
    };

    struct DataStorageOperation {
        std::string operation;
        json value;

        friend void to_json(nlohmann::json &j, const DataStorageOperation &op)
        {
            j = nlohmann::json{
                {"operation", op.operation},
                {"value", op.value}
            };
        }
    };

    void set_socket_connected_handler(std::function<void(void)> f)
    {
        _hOnSocketConnected = f;
    }

    void set_socket_error_handler(std::function<void(const std::string&)> f)
    {
        _hOnSocketError = f;
    }

    void set_socket_disconnected_handler(std::function<void(void)> f)
    {
        _hOnSocketDisconnected = f;
    }

    void set_slot_connected_handler(std::function<void(const json&)> f)
    {
        _hOnSlotConnected = f;
    }

    void set_slot_refused_handler(std::function<void(const std::list<std::string>&)> f)
    {
        _hOnSlotRefused = f;
    }

    void set_slot_disconnected_handler(std::function<void(void)> f)
    {
        _hOnSlotDisconnected = f;
    }

    void set_room_info_handler(std::function<void(void)> f)
    {
        _hOnRoomInfo = f;
    }

    void set_room_update_handler(std::function<void(void)> f)
    {
        _hOnRoomUpdate = f;
    }

    void set_items_received_handler(std::function<void(const std::list<NetworkItem>&)> f)
    {
        _hOnItemsReceived = f;
    }

    void set_location_info_handler(std::function<void(const std::list<NetworkItem>&)> f)
    {
        _hOnLocationInfo = f;
    }

    void set_data_package_changed_handler(std::function<void(const json&)> f)
    {
        _hOnDataPackageChanged = f;
    }

    void set_print_handler(std::function<void(const std::string&)> f)
    {
        _hOnPrint = f;
    }

    void set_print_json_handler(std::function<void(const json& command)> f)
    {
        _hOnPrintJson = f;
    }

    void set_print_json_handler(std::function<void(const PrintJSONArgs&)> f)
    {
        set_print_json_handler([f](const json& command) {
            if (!f) return;

            PrintJSONArgs args;

            int receiving;
            NetworkItem item;
            bool found;
            int team;
            int slot;
            std::string message;
            std::list<std::string> tags;
            int countdown;

            for (const auto& part: command["data"]) {
                args.data.push_back(TextNode::from_json(part));
            }

            args.type = command.value("type", "");

            auto it = command.find("receiving");
            if (it != command.end()) {
               receiving = *it;
               args.receiving = &receiving;
            }

            it = command.find("item");
            if (it != command.end()) {
                item.item = it->value("item", (int64_t) 0);
                item.location = it->value("location", (int64_t) 0);
                item.player = it->value("player", 0);
                item.flags = it->value("flags", 0U);
                item.index = -1;
                args.item = &item;
            }

            it = command.find("found");
            if (it != command.end()) {
                found = *it;
                args.found = &found;
            }

            it = command.find("team");
            if (it != command.end()) {
                team = *it;
                args.team = &team;
            }

            it = command.find("slot");
            if (it != command.end()) {
                slot = *it;
                args.slot = &slot;
            }

            it = command.find("message");
            if (it != command.end()) {
                message = *it;
                args.message = &message;
            }

            it = command.find("tags");
            if (it != command.end()) {
                it->get_to(tags);
                args.tags = &tags;
            }

            it = command.find("countdown");
            if (it != command.end()) {
                countdown = *it;
                args.countdown = &countdown;
            }

            f(args);
        });
    }

    void set_print_json_handler(std::function<void(const std::list<TextNode>&, const NetworkItem*, const int*)> f)
    {
        set_print_json_handler([f](const PrintJSONArgs& args) {
            if (!f) return;
            f(args.data, args.item, args.receiving);
        });
    }

    void set_print_json_handler(std::function<void(const std::list<TextNode>&)> f)
    {
        set_print_json_handler([f](const json& command) {
            if (!f) return;

            std::list<TextNode> data;

            for (const auto& part: command["data"]) {
                data.push_back(TextNode::from_json(part));
            }

            f(data);
        });
    }

    void set_print_json_handler(std::function<void(const std::list<TextNode>&, const json& extra)> f)
    {
        set_print_json_handler([f](const json& command) {
            if (!f)
                return;

            std::list<TextNode> data;
            json extra;

            for (const auto& part: command["data"]) {
                data.push_back(TextNode::from_json(part));
            }

            for (const auto& pair: command.items()) {
                extra[pair.key()] = pair.value();
            }

            f(data, extra);
        });
    }

    void set_bounced_handler(std::function<void(const json&)> f)
    {
        _hOnBounced = f;
    }

    void set_location_checked_handler(std::function<void(const std::list<int64_t>&)> f)
    {
        _hOnLocationChecked = f;
    }

    void set_retrieved_handler(std::function<void(const std::map<std::string,json>&)> f)
    {
        set_retrieved_handler([f](const std::map<std::string, json>& keys, const json&) {
            if (!f)
                return;

            f(keys);
        });
    }

    void set_retrieved_handler(std::function<void(const std::map<std::string,json>&, const json& message)> f)
    {
        _hOnRetrieved = f;
    }

    void set_set_reply_handler(std::function<void(const json& command)> f)
    {
        _hOnSetReply = f;
    }

    void set_set_reply_handler(std::function<void(const std::string&, const json&, const json&)> f) {
        set_set_reply_handler([f](const json& command) {
            if (!f) return;
            f(command["key"].get<std::string>(), command["value"], command["original_value"]);
        });
    }

    const std::set<int64_t> get_checked_locations() const
    {
        return _checkedLocations;
    }

    const std::set<int64_t> get_missing_locations() const
    {
        return _missingLocations;
    }

    const std::list<NetworkPlayer>& get_players() const
    {
        return _players;
    }

    std::string get_player_alias(int slot)
    {
        if (slot == 0)
            return "Server";

        for (const auto& player: _players) {
            if (player.team == _team && player.slot == slot) {
                return player.alias;
            }
        }

        return "Unknown";
    }

    const std::string& get_player_game(int player)
    {
        static const std::string ARCHIPELAGO_STRING = "Archipelago";
        static const std::string BLANK = "";

        if (player == 0)
            return ARCHIPELAGO_STRING;

        auto slotIt = _slotInfo.find(player);
        if (slotIt != _slotInfo.end())
            return slotIt->second.game;

        return BLANK;
    }

    /// Get the currently played game name or an empty string
    const std::string& get_game()
    {
        return get_player_game(get_player_number());
    }

    std::string get_location_name(int64_t code, const std::string& game)
    {
        if (game.empty()) { // old code path ("global" ids)
            auto it = _locations.find(code);
            if (it != _locations.end())
                return it->second;
        } else {
            auto locationsIt = _gameLocations.find(game);
            if (locationsIt != _gameLocations.end()) {
                auto it = locationsIt->second.find(code);
                if (it != locationsIt->second.end()) {
                    return it->second;
                }
            }
        }
        return "Unknown";
    }

    /**
     * Usage is not recommended
     * Return the id associated with the location name
     * Return APClient::INVALID_NAME_ID when undefined
     */
    int64_t get_location_id(const std::string& name) const
    {
        if (_dataPackage["games"].contains(_game)) {
            for (const auto& pair: _dataPackage["games"][_game]["location_name_to_id"].items()) {
                if (pair.key() == name)
                    return pair.value().get<int64_t>();
            }
        }
        return INVALID_NAME_ID;
    }

    std::string get_item_name(int64_t code, const std::string& game)
    {
        if (game.empty()) { // old code path ("global" ids)
            auto it = _items.find(code);
            if (it != _items.end())
                return it->second;
        } else {
            auto itemsIt = _gameItems.find(game);
            if (itemsIt != _gameItems.end()) {
                auto it = itemsIt->second.find(code);
                if (it != itemsIt->second.end()) {
                    return it->second;
                }
            }
        }
        return "Unknown";
    }

    /**
     * Usage is not recommended
     * Return the id associated with the item name
     * Return APClient::INVALID_NAME_ID when undefined
     */
    int64_t get_item_id(const std::string& name) const
    {
        if (_dataPackage["games"].contains(_game)) {
            for (const auto& pair: _dataPackage["games"][_game]["item_name_to_id"].items()) {
                if (pair.key() == name)
                    return pair.value().get<int64_t>();
            }
        }
        return INVALID_NAME_ID;
    }

    bool slot_concerns_self(int slot) const
    {
        if (slot == _slotnr)
            return true;
        auto it = _slotInfo.find(slot);
        if (it != _slotInfo.end()) {
            const auto& members = it->second.members;
            return std::find(members.begin(), members.end(), _slotnr) != members.end();
        }
        return false;
    }

    std::string render_json(const std::list<TextNode>& msg, RenderFormat fmt = RenderFormat::TEXT)
    {
        // TODO: implement RenderFormat::HTML
        if (fmt == RenderFormat::HTML)
            throw std::invalid_argument("AP::render_json(..., HTML) not implemented");
        std::string out;
        bool colorIsSet = false;
        for (const auto& node: msg) {
            std::string color;
            std::string text;
            if (fmt != RenderFormat::TEXT) color = node.color;
            if (node.type == "player_id") {
                int id = std::stoi(node.text);
                if (color.empty() && slot_concerns_self(id)) color = "magenta";
                else if (color.empty()) color = "yellow";
                text = get_player_alias(id);
            } else if (node.type == "item_id") {
                int64_t id = stoi64(node.text);
                if (color.empty()) {
                    if (node.flags & ItemFlags::FLAG_ADVANCEMENT) color = "plum";
                    else if (node.flags & ItemFlags::FLAG_NEVER_EXCLUDE) color = "slateblue";
                    else if (node.flags & ItemFlags::FLAG_TRAP) color = "salmon";
                    else color = "cyan";
                }
                text = get_item_name(id, get_player_game(node.player));
            } else if (node.type == "location_id") {
                int64_t id = stoi64(node.text);
                if (color.empty()) color = "blue";
                text = get_location_name(id, get_player_game(node.player));
            } else if (node.type == "hint_status") {
                text = node.text;
                if (node.hintStatus == HINT_FOUND) color = "green";
                else if (node.hintStatus == HINT_UNSPECIFIED) color = "grey";
                else if (node.hintStatus == HINT_NO_PRIORITY) color = "slateblue";
                else if (node.hintStatus == HINT_AVOID) color = "salmon";
                else if (node.hintStatus == HINT_PRIORITY) color = "plum";
                else color = "red";  // unknown status -> red
            } else {
                text = node.text;
            }
            if (fmt == RenderFormat::ANSI) {
                if (color.empty() && colorIsSet) {
                    out += color2ansi(""); // reset color
                    colorIsSet = false;
                } else if (!color.empty()) {
                    out += color2ansi(color);
                    colorIsSet = true;
                }
                deansify(text);
                out += text;
            } else {
                out += text;
            }
        }
        if (fmt == RenderFormat::ANSI && colorIsSet) out += color2ansi("");
        return out;
    }

    bool LocationChecks(std::list<int64_t> locations)
    {
        // returns true if checks were sent or queued
        if (_state == State::SLOT_CONNECTED) {
            auto packet = json{{
                {"cmd", "LocationChecks"},
                {"locations", locations},
            }};

            debug("> " + packet[0]["cmd"].get<std::string>() + ": " + packet.dump());
            _ws->send(packet.dump());
        } else {
            _checkQueue.insert(locations.begin(), locations.end());
        }
        for (const auto& location: locations) {
            _checkedLocations.insert(location);
            _missingLocations.erase(location);
        }
        return true;
    }

    bool LocationScouts(std::list<int64_t> locations, int create_as_hint = 0)
    {
        // returns true if scouts were sent or queued
        if (_state == State::SLOT_CONNECTED) {
            auto packet = json{{
                {"cmd", "LocationScouts"},
                {"locations", locations},
                {"create_as_hint", create_as_hint},
            }};

            debug("> " + packet[0]["cmd"].get<std::string>() + ": " + packet.dump());
            _ws->send(packet.dump());
        } else {
            _scoutQueues[create_as_hint].insert(locations.begin(), locations.end());
        }
        return true;
    }

    /**
     * Sends UpdateHint to the server to update hint status/priority.
     * Returns true if hint update was sent or queued.
     */
    bool UpdateHint(int player, int64_t location, HintStatus status)
    {
        if (_state == State::SLOT_CONNECTED) {
            auto packet = json{{
                {"cmd", "UpdateHint"},
                {"player", player},
                {"location", location},
                {"status", status},
            }};

            debug("> " + packet[0]["cmd"].get<std::string>() + ": " + packet.dump());
            _ws->send(packet.dump());
        } else {
            _updateHintQueue.emplace_back(player, location, status);
        }
        return true;
    }

    bool StatusUpdate(ClientStatus status)
    {
        // returns true if status update was sent or queued
        if (_state == State::SLOT_CONNECTED) {
            auto packet = json{{
                {"cmd", "StatusUpdate"},
                {"status", status},
            }};

            debug("> " + packet[0]["cmd"].get<std::string>() + ": " + packet.dump());
            _ws->send(packet.dump());
            return true;
        }

        _clientStatus = status;
        return false;
    }

    bool ConnectSlot(const std::string& name, const std::string& password, int items_handling,
                     const std::list<std::string>& tags = {}, const Version& ver = APCLIENTPP_VERSION_INITIALIZER)
    {
        if (_state < State::SOCKET_CONNECTED)
            return false;

        _slot = name;
        debug("Connecting slot...");
        auto packet = json{{
            {"cmd", "Connect"},
            {"game", _game},
            {"uuid", _uuid},
            {"name", name},
            {"password", password},
            {"version", ver},
            {"items_handling", items_handling},
            {"tags", tags},
        }};

        debug("> " + packet[0]["cmd"].get<std::string>() + ": " + packet.dump());
        _ws->send(packet.dump());
        return true;
    }

#if defined __cpp_lib_optional || defined __cpp_lib_experimental_optional
#   if defined __cpp_lib_optional
    template<class T>
    using optional = std::optional<T>;
#   else
    template<class T>
    using optional = std::experimental::optional<T>;
#   endif

    bool ConnectUpdate(optional<int> items_handling, optional<const std::list<std::string>> tags)
    {
        return ConnectUpdate((bool)items_handling, *items_handling, (bool)tags, *tags);
    }
#endif

    bool ConnectUpdate(bool send_items_handling, int items_handling, bool send_tags, const std::list<std::string>& tags)
    {
        if (!send_items_handling && !send_tags)
            return false;

        auto packet = json{{
            {"cmd", "ConnectUpdate"},
        }};

        if (send_items_handling) packet[0]["items_handling"] = items_handling;
        if (send_tags) packet[0]["tags"] = tags;

        debug("> " + packet[0]["cmd"].get<std::string>() + ": " + packet.dump());
        _ws->send(packet.dump());
        return true;
    }

    bool Sync()
    {
        if (_state < State::SLOT_CONNECTED)
            return false;

        auto packet = json{{
            {"cmd", "Sync"},
        }};

        debug("> " + packet[0]["cmd"].get<std::string>() + ": " + packet.dump());
        _ws->send(packet.dump());
        return true;
    }

    bool GetDataPackage(const std::list<std::string>& include)
    {
        if (_state < State::ROOM_INFO)
            return false;

        if (_serverVersion < Version{0, 3, 2}) {
            const char* msg = "GetDataPackage for AP before 0.3.2 is not supported anymore";
            fprintf(stderr, "APClient: %s!\n", msg);
#ifdef __cpp_exceptions
            throw std::runtime_error(msg);
#else
            return false;
#endif
        }

        // optimized data package fetching:
        // fetch in multiple packets for better streaming / less blocking
        // fetch in at least 2 steps if more than 1 game needs to be fetched
        // prefer to fetch 2 games at once for better use of compression window
        // if it's an odd number, the last fetch should be 1 game
        size_t n = 0;
        size_t count = include.size();
        std::vector<std::string> games;
        for (const auto& game: include) {
            games.push_back(game);
            n++;
            if (count > 2 && n != count && (n % 2) != 0) {
                continue;
            }
            auto packet = json{{
                {"cmd", "GetDataPackage"},
                {"games", games}, // since 0.3.2
            }};
            debug("> " + packet[0]["cmd"].get<std::string>() + ": " + packet.dump());
            _ws->send(packet.dump());
            _pendingDataPackageRequests++;
            games.clear();
        }

        return true;
    }

    bool Bounce(const json& data, std::list<std::string> games = {},
                std::list<int> slots = {}, std::list<std::string> tags = {})
    {
        if (_state < State::ROOM_INFO)
            return false; // or SLOT_CONNECTED?

        auto packet = json{{
            {"cmd", "Bounce"},
            {"data", data},
        }};

        if (!games.empty()) packet[0]["games"] = games;
        if (!slots.empty()) packet[0]["slots"] = slots;
        if (!tags.empty()) packet[0]["tags"] = tags;

#ifdef APCLIENT_DEBUG
        const size_t maxDumpLen = 512;
        auto dump = packet[0].dump().substr(0, maxDumpLen);
        if (dump.size() > maxDumpLen-3) dump = dump.substr(0, maxDumpLen-3) + "...";
        debug("> " + packet[0]["cmd"].get<std::string>() + ": " + dump);
#endif
        _ws->send(packet.dump());
        return true;
    }

    bool Say(const std::string& text)
    {
        if (_state < State::ROOM_INFO) // or SLOT_CONNECTED?
            return false;

        auto packet = json{{
            {"cmd", "Say"},
            {"text", text},
        }};
        debug("> " + packet[0]["cmd"].get<std::string>() + ": " + packet.dump());
        _ws->send(packet.dump());

        return true;
    }

    bool Get(const std::list<std::string>& keys, const json& extras = json::value_t::object)
    {
        if (_state < State::SLOT_CONNECTED)
            return false;

        auto packet = json{{
            {"cmd", "Get"},
            {"keys", keys},
        }};

        if (!extras.is_null())
            packet[0].update(extras);

        debug("> " + packet[0]["cmd"].get<std::string>() + ": " + packet.dump());
        _ws->send(packet.dump());
        return true;
    }

    bool Set(const std::string& key, const json& dflt, bool want_reply,
             const std::list<DataStorageOperation>& operations, const json& extras = json::value_t::object)
    {
        if (_state < State::SLOT_CONNECTED)
            return false;

        auto packet = json{{
           {"cmd", "Set"},
           {"key", key},
           {"default", dflt},
           {"want_reply", want_reply},
           {"operations", operations},
        }};

        if (!extras.is_null())
            packet[0].update(extras);

        debug("> " + packet[0]["cmd"].get<std::string>() + ": " + packet.dump());
        _ws->send(packet.dump());
        return true;
    }

    bool SetNotify(const std::list<std::string>& keys)
    {
        if (_state < State::SLOT_CONNECTED)
            return false;

        auto packet = json{{
            {"cmd", "SetNotify"},
            {"keys", keys},
        }};

        debug("> " + packet[0]["cmd"].get<std::string>() + ": " + packet.dump());
        _ws->send(packet.dump());
        return true;
    }

    State get_state() const
    {
        return _state;
    }

    bool has_password() const
    {
        return _hasPassword;
    }

    const std::string& get_seed() const
    {
        return _seed;
    }

    const std::string& get_slot() const
    {
        return _slot;
    }

    int get_player_number() const
    {
        return _slotnr;
    }

    int get_team_number() const
    {
        return _team;
    }

    /// Get current hint points for the connect slot. This might wrongly return 0 for servers before merging #1548
    int get_hint_points() const
    {
        return _hintPoints;
    }

    /// Get cost of a hint in points for the connect slot.
    int get_hint_cost_points() const
    {
        if (!_hintCostPercent)
            return 0;
        if (_serverVersion >= Version{0, 3, 9})
            return std::max(1, _hintCostPercent * _locationCount / 100);
        return _hintCostPercent * _locationCount / 100;
    }

    /// Get cost of a hint in percent of total location count for the connected server.
    int get_hint_cost_percent() const
    {
        return _hintCostPercent;
    }

    /**
     * Checks if data package seems to be valid for the server/room.
     * If not, get_location_name() and get_item_name() will return "Unknown"
     */
    bool is_data_package_valid() const
    {
        return _dataPackageValid;
    }

    /// Get the estimated server Unix time stamp as double. Useful to filter deathlink
    double get_server_time() const
    {
        auto td = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - _localConnectTime);
        return _serverConnectTime + td.count();
    }

    /// Get the version of the server currently connected to
    Version get_server_version() const
    {
        return _serverVersion;
    }

    /// Get the version of AP that generated the game connected to
    Version get_generator_version() const
    {
        return _generatorVersion;
    }

    /**
     * Poll the network layer and dispatch callbacks.
     * This has to be called repeatedly (i.e. once per frame) while this object exists.
     */
    void poll()
    {
        if (_ws && _state == State::DISCONNECTED)
            _ws.reset();
        if (_ws)
            _ws->poll();
        if (_state < State::SOCKET_CONNECTED) {
            auto t = now();
            if (t - _lastSocketConnect > _socketReconnectInterval || _reconnectNow) {
                if (_state != State::DISCONNECTED)
                    log("Connect timed out. Retrying.");
                else
                    log("Reconnecting to server");
                connect_socket();
            }
        }
    }

    /// Clear all state and reconnect on next poll
    void reset()
    {
        _checkQueue.clear();
        _scoutQueues.clear();
        _updateHintQueue.clear();
        _clientStatus = ClientStatus::UNKNOWN;
        _seed.clear();
        _slot.clear();
        _team = -1;
        _slotnr = -1;
        _locationCount = 0;
        _hintCostPercent = 0;
        _hintPoints = 0;
        _players.clear();
        _ws.reset();
        _state = State::DISCONNECTED;
        _hasPassword = false;
    }

private:
    void log(const char* msg)
    {
        printf("APClient: %s\n", msg);
    }

    void log(const std::string& msg)
    {
        log(msg.c_str());
    }

    void debug(const char* msg)
    {
#ifdef APCLIENT_DEBUG
        log(msg);
#else
        (void)msg;
#endif
    }

    void debug(const std::string& msg)
    {
        debug(msg.c_str());
    }

    void onopen()
    {
        debug("onopen()");
        log("Server connected");
        _state = State::SOCKET_CONNECTED;
        _pendingDataPackageRequests = 0;
        _serverVersion = _generatorVersion = Version{0, 0, 0};
        if (_hOnSocketConnected) _hOnSocketConnected();
        _socketReconnectInterval = 1500;
    }

    void onclose()
    {
        debug("onclose()");
        if (_state > State::SOCKET_CONNECTING) {
            log("Server disconnected");
            _state = State::DISCONNECTED;
            if (_hOnSocketDisconnected) _hOnSocketDisconnected();
        }
        _state = State::DISCONNECTED;
        _seed = "";
    }

    void onmessage(const std::string& s)
    {
        try {
            json packet = json::parse(s);
#ifndef AP_NO_SCHEMA
            valijson::Validator validator;
            JsonSchemaAdapter packetAdapter(packet);
            if (!validator.validate(_packetSchema, packetAdapter, nullptr)) {
                throw std::runtime_error("Packet validation failed");
            }
#endif
            for (auto& command: packet) {
                std::string cmd = command["cmd"];
#ifndef AP_NO_SCHEMA
                JsonSchemaAdapter commandAdapter(command);
                auto schemaIt = _commandSchemas.find(cmd);
                if (schemaIt != _commandSchemas.end()) {
                    if (!validator.validate(schemaIt->second, commandAdapter, nullptr)) {
                        throw std::runtime_error("Command validation failed");
                    }
                }
#endif
#ifdef APCLIENT_DEBUG
                const size_t maxDumpLen = 512;
                auto dump = command.dump().substr(0, maxDumpLen);
                if (dump.size() > maxDumpLen-3) dump = dump.substr(0, maxDumpLen-3) + "...";
                debug("< " + cmd + ": " + dump);
#endif
                if (cmd == "RoomInfo") {
                    _localConnectTime = std::chrono::steady_clock::now();
                    _serverConnectTime = command["time"].get<double>();
                    _serverVersion = Version::from_json(command["version"]);
                    _generatorVersion = Version::from_json(command["generator_version"]);
                    _seed = command["seed_name"];
                    _hintCostPercent = command.value("hint_cost", 0);
                    _hasPassword = command.value("password", false);
                    if (_state < State::ROOM_INFO) _state = State::ROOM_INFO;
                    if (_hOnRoomInfo) _hOnRoomInfo();

                    // check if cached data package is already valid
                    // if not, build a list to query
                    _dataPackageValid = true;
                    std::list<std::string> exclude;
                    std::list<std::string> include;
                    std::set<std::string> playedGames;
                    auto itGames = command.find("games");
                    if (itGames != command.end() && itGames->is_array()) {
                        // 0.2.0+: use games list, always include "Archipelago"
                        playedGames = itGames->get<std::set<std::string>>();
                        playedGames.emplace("Archipelago");
                    } else if (command["datapackage_versions"].is_array()) {
                        // 0.1.x: get games from datapackage_versions
                        for (auto itV: command["datapackage_versions"].items()) {
                            playedGames.emplace(itV.key());
                        }
                    } else {
                        // alpha: summed datapackage_version, not supported, always fetch all
                        _dataPackageValid = false;
                    }

                    auto itVersions = command.find("datapackage_versions");
                    if (itVersions != command.end() && !itVersions->is_object()) itVersions = command.end();
                    auto itChecksums = command.find("datapackage_checksums");
                    if (itChecksums != command.end() && !itChecksums->is_object()) itChecksums = command.end();

                    if (itVersions != command.end() && !playedGames.empty()) {
                        // pre 0.3.2: exclude games that exist but are not being played
                        for (auto itV: command["datapackage_versions"].items()) {
                            if (!playedGames.count(itV.key())) {
                                exclude.push_back(itV.key());
                            }
                        }
                    }

                    for (const auto& game: playedGames) {
                        std::string remoteChecksum;
                        int remoteVersion = 0;
                        if (itChecksums != command.end()) {
                            auto itChecksum = itChecksums->find(game);
                            if (itChecksum != itChecksums->end() && itChecksum->is_string())
                                remoteChecksum = *itChecksum;
                        }
                        if (itVersions != command.end()) {
                            auto itVersion = itVersions->find(game);
                            if (itVersion != itVersions->end() && itVersion->is_number_integer())
                                remoteVersion = *itVersion;
                        }
                        json localData;
                        if (!_dataPackageStore || !_dataPackageStore->load(game, remoteChecksum, localData)) {
                            if (remoteChecksum.empty() && remoteVersion != 0) {
                                auto itOld = _dataPackage["games"].find(game);
                                if (itOld != _dataPackage["games"].end()) {
                                    // exists in migrated cache
                                    auto itOldVersion = itOld->find("version");
                                    if (itOldVersion != itOld->end() && *itOldVersion == remoteVersion) {
                                        // and is recent
                                        exclude.push_back(game);
                                        continue;
                                    }
                                }
                            }
                            include.push_back(game);
                            _dataPackageValid = false;
                        } else if (!remoteChecksum.empty()) {
                            // compare checksum
                            auto it = localData.find("checksum");
                            if (it != localData.end() && it->is_string() && *it == remoteChecksum) {
                                _dataPackage["games"][game] = localData;
                                exclude.push_back(game);
                            } else {
                                include.push_back(game);
                                _dataPackageValid = false;
                            }
                        } else {
                            const auto it = localData.find("version");
                            if (remoteVersion != 0 && it != localData.end() && it->is_number_integer() && *it == remoteVersion) {
                                _dataPackage["games"][game] = localData;
                                exclude.push_back(game);
                            } else {
                                include.push_back(game);
                                _dataPackageValid = false;
                            }
                        }
                    }

                    if (!exclude.empty())
                        _set_data_package(_dataPackage);  // apply loaded strings
                    if (!_dataPackageValid) GetDataPackage(include);
                    else debug("Data package up to date");
                }
                else if (cmd == "ConnectionRefused") {
                    if (_hOnSlotRefused) {
                        std::list<std::string> errors;
                        for (const auto& error: command["errors"])
                            errors.push_back(error);
                        _hOnSlotRefused(errors);
                    }
                }
                else if (cmd == "Connected") {
                    // store data
                    _state = State::SLOT_CONNECTED;
                    _team = command["team"];
                    _slotnr = command["slot"];
                    _hintPoints = command.value("hint_points", command["checked_locations"].size());
                    _locationCount = command["missing_locations"].size() + command["checked_locations"].size();
                    _players.clear();
                    for (auto& player: command["players"]) {
                        _players.push_back({
                            player["team"].get<int>(),
                            player["slot"].get<int>(),
                            player["alias"].get<std::string>(),
                            player["name"].get<std::string>(),
                        });
                    }
                    _checkedLocations = command.value<std::set<int64_t>>("checked_locations", {});
                    _missingLocations = command.value<std::set<int64_t>>("missing_locations", {});
                    // send queued checks if any - this makes sure checked/missing is up to date
                    if (!_checkQueue.empty()) {
                        std::list<int64_t> queuedChecks;
                        for (int64_t location : _checkQueue) {
                            queuedChecks.push_back(location);
                        }
                        _checkQueue.clear();
                        LocationChecks(queuedChecks);
                    }
                    if (command["slot_info"].is_object()) {
                        for (auto it: command["slot_info"].items()) {
                            NetworkSlot slot;
                            const auto& j = it.value();
                            j.at("name").get_to(slot.name);
                            j.at("game").get_to(slot.game);
                            j.at("type").get_to(slot.type);
                            j.at("group_members").get_to(slot.members);
                            int player = atoi(it.key().c_str());
                            _slotInfo[player] = slot;
                        }
                    }
                    // run the callbacks
                    if (_hOnSlotConnected)
                        _hOnSlotConnected(command["slot_data"]);
                    if (_hOnLocationChecked) {
                        std::list<int64_t> checkedLocations;
                        for (auto& location: command["checked_locations"]) {
                            checkedLocations.push_back(location.get<int64_t>());
                        }
                        if (!checkedLocations.empty())
                            _hOnLocationChecked(checkedLocations);
                    }
                    // send queued scouts if any
                    if (!_scoutQueues.empty()) {
                        for (const auto& pair: _scoutQueues) {
                            if (!pair.second.empty()) {
                                std::list<int64_t> queuedScouts;
                                for (int64_t location : pair.second) {
                                    queuedScouts.push_back(location);
                                }
                                LocationScouts(queuedScouts, pair.first);
                            }
                        }
                        _scoutQueues.clear();
                    }
                    // send queued hint updates, if any
                    auto hintUpdates = std::move(_updateHintQueue);
                    for (auto& hintUpdate: hintUpdates) {
                        UpdateHint(std::get<0>(hintUpdate), std::get<1>(hintUpdate), std::get<2>(hintUpdate));
                    }
                }
                else if (cmd == "ReceivedItems") {
                    std::list<NetworkItem> items;
                    int index = command["index"].get<int>();
                    for (const auto& j: command["items"]) {
                        NetworkItem item;
                        item.item = j["item"].get<int64_t>();
                        item.location = j["location"].get<int64_t>();
                        item.player = j["player"].get<int>();
                        item.flags = j.value("flags", 0U);
                        item.index = index++;
                        items.push_back(item);
                    }
                    if (_hOnItemsReceived) _hOnItemsReceived(items);
                }
                else if (cmd == "LocationInfo") {
                    std::list<NetworkItem> items;
                    for (const auto& j: command["locations"]) {
                        NetworkItem item;
                        item.item = j["item"].get<int64_t>();
                        item.location = j["location"].get<int64_t>();
                        item.player = j["player"].get<int>();
                        item.flags = j.value("flags", 0U);
                        item.index = -1;
                        items.push_back(item);
                    }
                    if (_hOnLocationInfo) _hOnLocationInfo(items);
                }
                else if (cmd == "RoomUpdate") {
                    std::list<int64_t> checkedLocations;
                    for (const auto& j: command["checked_locations"]) {
                        int64_t location = j.get<int64_t>();
                        if (_checkedLocations.emplace(location).second) {
                            checkedLocations.push_back(location);
                            _missingLocations.erase(location);
                        }
                    }
                    if (_hOnLocationChecked && !checkedLocations.empty())
                        _hOnLocationChecked(checkedLocations);
                    if (command["hint_points"].is_number_integer())
                        _hintPoints = command["hint_points"];
                    if (command["players"].is_array()) {
                        _players.clear();
                        for (auto& player: command["players"]) {
                            _players.push_back({
                                player["team"].get<int>(),
                                player["slot"].get<int>(),
                                player["alias"].get<std::string>(),
                                player["name"].get<std::string>(),
                            });
                        }
                    }
                    if (_hOnRoomUpdate)
                        _hOnRoomUpdate();
                }
                else if (cmd == "DataPackage") {
                    auto data = _dataPackage;
                    if (!data["games"].is_object())
                        data["games"] = json(json::value_t::object);
                    for (auto gamepair: command["data"]["games"].items()) {
                        if (_dataPackageStore)
                            _dataPackageStore->save(gamepair.key(), gamepair.value());
                        data["games"][gamepair.key()] = gamepair.value();
                    }
                    data["version"] = command["data"].value<int>("version", -1); // -1 for backwards compatibility
                    _dataPackageValid = false;
                    _set_data_package(data);
                    if (_pendingDataPackageRequests > 0) {
                        _pendingDataPackageRequests--;
                        if (_pendingDataPackageRequests == 0) {
                            _dataPackageValid = true;
                            if (_hOnDataPackageChanged) _hOnDataPackageChanged(_dataPackage);
                        }
                    }
                }
                else if (cmd == "Print") {
                    if (_hOnPrint) _hOnPrint(command["text"].get<std::string>());
                }
                else if (cmd == "PrintJSON") {
                    if (_hOnPrintJson) _hOnPrintJson(command);
                }
                else if (cmd == "Bounced") {
                    if (_hOnBounced) _hOnBounced(command);
                }
                else if (cmd == "Retrieved") {
                    if (_hOnRetrieved) {
                        std::map<std::string, json> keys;
                        for (auto& pair: command["keys"].items())
                            keys[pair.key()] = pair.value();
                        _hOnRetrieved(keys, command);
                    }
                }
                else if (cmd == "SetReply") {
                    if (_hOnSetReply) {
                        command["original_value"]; // insert null if missing
                        _hOnSetReply(command);
                    }
                }
                else {
                    debug("unhandled cmd");
                }
            }
        } catch (const std::exception& ex) {
            log((std::string("onmessage() error: ") + ex.what()).c_str());
        }
    }

    void onerror(const std::string& msg = "")
    {
        debug("onerror(" + msg + ")");
        if (_hOnSocketError) _hOnSocketError(msg);
        // TODO: on desktop, we could check if the error was handle_read_http_response before switching to wss://
        //       and handle_transport_init before switching to ws://
        if (_tryWSS && _uri.rfind("ws://", 0) == 0) {
            _uri = "wss://" + _uri.substr(5);
            if (_state == State::SOCKET_CONNECTING)
                _reconnectNow = true; // force immediate connect attempt
        } else if (_tryWSS && _uri.rfind("wss://", 0) == 0) {
            _uri = "ws://" + _uri.substr(6);
        }
    }

    void connect_socket()
    {
        _reconnectNow = false;
        _ws.reset();
        if (_uri.empty()) {
            _ws = nullptr;
            _state = State::DISCONNECTED;
            return;
        }
        _state = State::SOCKET_CONNECTING;

        try {
            _ws.reset(new WS(_uri,
                    [this]() { onopen(); },
                    [this]() { onclose(); },
                    [this](const std::string& s) { onmessage(s); },
#if WSWRAP_VERSION >= 10200
                    [this](const std::string& s) { onerror(s); }
#else
                    [this]() { onerror(); }
#endif
#if WSWRAP_VERSION >= 10100
                    , _certStore
#endif
            ));
        } catch (const std::exception& ex) {
            _ws = nullptr;
            if (_tryWSS && _uri.rfind("ws://", 0) == 0) {
                _uri = "wss://" + _uri.substr(5);
            } else {
                _uri = "ws://" + _uri.substr(6);
            }
            log((std::string("error connecting: ") + ex.what()).c_str());
        }
        _lastSocketConnect = now();
        _socketReconnectInterval *= 2;
        // NOTE: browsers have a very badly implemented connection rate limit
        // alternatively we could always wait for onclose() to get the actual
        // allowed rate once we are over it
        unsigned long maxReconnectInterval = std::max(15000UL, _ws ? _ws->get_ok_connect_interval() : 0);
        if (_socketReconnectInterval > maxReconnectInterval)
            _socketReconnectInterval = maxReconnectInterval;
    }

    void _set_data_package(const json& data)
    {
        _dataPackage = data;
        for (auto gamepair: _dataPackage["games"].items()) {
            const auto& gamedata = gamepair.value();
            _dataPackage["games"][gamepair.key()] = gamedata;
            auto& gameItems = _gameItems[gamepair.key()];
            for (auto pair: gamedata["item_name_to_id"].items()) {
                auto id = pair.value().get<int64_t>();
                _items[id] = pair.key();
                gameItems[id] = pair.key();
            }
            auto& gameLocations = _gameLocations[gamepair.key()];
            for (auto pair: gamedata["location_name_to_id"].items()) {
                auto id = pair.value().get<int64_t>();
                _locations[id] = pair.key();
                gameLocations[id] = pair.key();
            }
        }
    }

    std::string color2ansi(const std::string& color)
    {
        // convert color to ansi color command
        if (color == "red") return "\x1b[31m";
        if (color == "green") return "\x1b[32m";
        if (color == "yellow") return "\x1b[33m";
        if (color == "blue") return "\x1b[34m";
        if (color == "magenta") return "\x1b[35m";
        if (color == "cyan") return "\x1b[36m";
        if (color == "plum") return "\x1b[38:5:219m";
        if (color == "slateblue") return "\x1b[38:5:62m";
        if (color == "salmon") return "\x1b[38:5:210m";
        if (color == "gray") return "\x1b[90m";
        if (color == "grey") return "\x1b[90m";
        return "\x1b[0m";
    }

    void deansify(std::string& text)
    {
        // disable ansi commands in text by replacing ESC by space
        std::replace(text.begin(), text.end(), '\x1b', ' ');
    }

    static unsigned long now()
    {
#if defined WIN32 || defined _WIN32
        return (unsigned long)GetTickCount();
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        unsigned long ms = (unsigned long)ts.tv_sec * 1000;
        ms += (unsigned long)ts.tv_nsec / 1000000;
        return ms;
#endif
    }

    std::string _uri;
    std::string _game;
    std::string _uuid;
    std::string _certStore;
    std::unique_ptr<WS> _ws;
    State _state = State::DISCONNECTED;
    bool _tryWSS = false;

    std::function<void(void)> _hOnSocketConnected = nullptr;
    std::function<void(const std::string&)> _hOnSocketError = nullptr;
    std::function<void(void)> _hOnSocketDisconnected = nullptr;
    std::function<void(const json&)> _hOnSlotConnected = nullptr;
    std::function<void(void)> _hOnSlotDisconnected = nullptr;
    std::function<void(const std::list<std::string>&)> _hOnSlotRefused = nullptr;
    std::function<void(void)> _hOnRoomInfo = nullptr;
    std::function<void(void)> _hOnRoomUpdate = nullptr;
    std::function<void(const std::list<NetworkItem>&)> _hOnItemsReceived = nullptr;
    std::function<void(const std::list<NetworkItem>&)> _hOnLocationInfo = nullptr;
    std::function<void(const json&)> _hOnDataPackageChanged = nullptr;
    std::function<void(const std::string&)> _hOnPrint = nullptr;
    std::function<void(const json&)> _hOnPrintJson = nullptr;
    std::function<void(const json&)> _hOnBounced = nullptr;
    std::function<void(const std::list<int64_t>&)> _hOnLocationChecked = nullptr;
    std::function<void(const std::map<std::string, json>&, const json&)> _hOnRetrieved = nullptr;
    std::function<void(const json&)> _hOnSetReply = nullptr;

    unsigned long _lastSocketConnect;
    unsigned long _socketReconnectInterval = 1500;
    bool _reconnectNow = false;
    std::set<int64_t> _checkQueue;
    std::map<int, std::set<int64_t>> _scoutQueues;
    std::vector<std::tuple<int, int64_t, HintStatus>> _updateHintQueue;
    ClientStatus _clientStatus = ClientStatus::UNKNOWN;
    std::string _seed;
    std::string _slot; // currently connected slot, if any
    bool _hasPassword = false;
    int _team = -1;
    int _slotnr = -1;
    std::list<NetworkPlayer> _players;
    std::map<int64_t, std::string> _locations;
    std::map<int64_t, std::string> _items;
    std::map<std::string, std::map<int64_t, std::string>> _gameLocations;
    std::map<std::string, std::map<int64_t, std::string>> _gameItems;
    bool _dataPackageValid = false;
    size_t _pendingDataPackageRequests = 0;
    json _dataPackage;
    double _serverConnectTime = 0;
    std::chrono::steady_clock::time_point _localConnectTime;
    Version _serverVersion = {0,0,0};
    Version _generatorVersion = {0,0,0};
    int _locationCount = 0;
    int _hintCostPercent = 0;
    int _hintPoints = 0;
    std::set<int64_t> _checkedLocations;
    std::set<int64_t> _missingLocations;
    APDataPackageStore* _dataPackageStore;
#ifndef AP_NO_DEFAULT_DATA_PACKAGE_STORE
    std::unique_ptr<APDataPackageStore> _autoDataPackageStore;
#endif
    std::map<int, NetworkSlot> _slotInfo;

#ifndef AP_NO_SCHEMA
    const json _packetSchemaJson = R"({
        "type": "array",
        "items": {
            "type": "object",
            "properties": {
                "cmd": { "type": "string" }
            },
            "required": [ "cmd" ]
        }
    })"_json;
    valijson::Schema _packetSchema;

    const json _retrievedSchemaJson = R"({
        "type": "object",
        "properties": {
            "keys": { "type": "object" }
        },
        "required": [ "keys" ]
    })"_json;
    const json _setReplySchemaJson = R"({
        "type": "object",
        "properties": {
            "key": { "type": "string" }
        },
        "required": [ "key", "value" ]
    })"_json;
    std::map<std::string, valijson::Schema> _commandSchemas;
#endif
};

#endif // _APCLIENT_HPP
