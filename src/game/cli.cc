#include "game/cli.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "game/actions.h"
#include "game/anim.h"
#include "game/art.h"
#include "game/combat.h"
#include "game/critter.h"
#include "game/display.h"
#include "game/editor.h"
#include "game/game.h"
#include "game/gdialog.h"
#include "game/intface.h"
#include "game/inventry.h"
#include "game/item.h"
#include "game/loadsave.h"
#include "game/mainmenu.h"
#include "game/map.h"
#include "game/map_defs.h"
#include "game/object.h"
#include "game/pipboy.h"
#include "game/proto.h"
#include "game/protinst.h"
#include "game/scripts.h"
#include "game/skill.h"
#include "game/stat.h"
#include "game/tile.h"
#include "game/trait.h"
#include "game/worldmap.h"
#include "game/party.h"
#include "plib/gnw/input.h"
#include "plib/gnw/kb.h"

namespace fallout {

namespace {

constexpr const char* kCliInputPipePath = "/tmp/fallout-cli-in";
constexpr const char* kCliOutputPath = "/tmp/fallout-cli-out.txt";
constexpr int kMaxDisplayLogLines = 8;
constexpr int kCliDebugObjectsPerElevationLimit = 50;
constexpr int kCliGotoMaxPathLength = 100;
constexpr int kCliGotoPathRotationsCapacity = 800;
constexpr unsigned int kCliGotoWaitTimeoutMs = 60000;
constexpr unsigned int kCliGotoWaitStepMs = 16;

struct CliCommandResponse {
    bool ok;
    std::string body;
};

struct NearbyObjectInfo {
    Object* object;
    int distance;
    int direction;
};

struct CliPathTrackingContext {
    bool active;
    int targetTile;
    int bestTile;
    int bestDistance;
};

bool gCliEnabled = false;
int gCliInputFd = -1;
std::string gCliInputBuffer;
CliPathTrackingContext gCliPathTrackingContext = { false, -1, -1, INT_MAX };

std::string cliTrim(const std::string& value)
{
    size_t start = 0;
    while (start < value.size() && isspace(static_cast<unsigned char>(value[start])) != 0) {
        start++;
    }

    size_t end = value.size();
    while (end > start && isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        end--;
    }

    return value.substr(start, end - start);
}

std::string cliToLower(std::string value)
{
    for (char& ch : value) {
        ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    }

    return value;
}

std::string cliNormalizeName(const std::string& value)
{
    std::string normalized;
    normalized.reserve(value.size());

    for (char ch : value) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (isalnum(uch) != 0) {
            normalized.push_back(static_cast<char>(tolower(uch)));
        }
    }

    return normalized;
}

std::string cliEscapeValue(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());

    for (char ch : value) {
        if (ch == '\n') {
            escaped += "\\n";
        } else if (ch == '\r') {
            escaped += "\\r";
        } else {
            escaped.push_back(ch);
        }
    }

    return escaped;
}

std::vector<std::string> cliTokenize(const std::string& line)
{
    std::vector<std::string> tokens;
    std::string token;

    bool inQuotes = false;
    char quoteChar = '\0';

    for (char ch : line) {
        if ((ch == '\'' || ch == '"')) {
            if (inQuotes && quoteChar == ch) {
                inQuotes = false;
                quoteChar = '\0';
                continue;
            }

            if (!inQuotes) {
                inQuotes = true;
                quoteChar = ch;
                continue;
            }
        }

        if (!inQuotes && isspace(static_cast<unsigned char>(ch)) != 0) {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
            continue;
        }

        token.push_back(ch);
    }

    if (!token.empty()) {
        tokens.push_back(token);
    }

    return tokens;
}

std::string cliJoinTokens(const std::vector<std::string>& tokens, size_t first)
{
    if (first >= tokens.size()) {
        return std::string();
    }

    std::ostringstream out;
    for (size_t index = first; index < tokens.size(); index++) {
        if (index != first) {
            out << ' ';
        }
        out << tokens[index];
    }

    return out.str();
}

bool cliParseInteger(const std::string& value, int* outValue)
{
    if (outValue == NULL) {
        return false;
    }

    char* end = NULL;
    long parsed = strtol(value.c_str(), &end, 0);
    if (end == value.c_str() || *end != '\0') {
        return false;
    }

    if (parsed < INT_MIN || parsed > INT_MAX) {
        return false;
    }

    *outValue = static_cast<int>(parsed);
    return true;
}

const char* cliDirectionToString(int rotation)
{
    switch (rotation) {
    case ROTATION_NE:
        return "ne";
    case ROTATION_E:
        return "e";
    case ROTATION_SE:
        return "se";
    case ROTATION_SW:
        return "sw";
    case ROTATION_W:
        return "w";
    case ROTATION_NW:
        return "nw";
    default:
        return "unknown";
    }
}

int cliDirectionFromString(const std::string& direction)
{
    std::string normalized = cliToLower(direction);

    if (normalized == "n") {
        return ROTATION_NE;
    }

    if (normalized == "s") {
        return ROTATION_SW;
    }

    if (normalized == "ne") {
        return ROTATION_NE;
    }

    if (normalized == "e") {
        return ROTATION_E;
    }

    if (normalized == "se") {
        return ROTATION_SE;
    }

    if (normalized == "sw") {
        return ROTATION_SW;
    }

    if (normalized == "w") {
        return ROTATION_W;
    }

    if (normalized == "nw") {
        return ROTATION_NW;
    }

    return -1;
}

const char* cliObjectTypeToString(Object* object)
{
    if (object == NULL) {
        return "unknown";
    }

    switch (PID_TYPE(object->pid)) {
    case OBJ_TYPE_CRITTER:
        return "critter";
    case OBJ_TYPE_ITEM:
        return "item";
    case OBJ_TYPE_SCENERY:
        return obj_is_a_portal(object) ? "door" : "scenery";
    case OBJ_TYPE_WALL:
        return "wall";
    case OBJ_TYPE_TILE:
        return "tile";
    case OBJ_TYPE_MISC:
        return "misc";
    default:
        return "unknown";
    }
}

const char* cliPidTypeToString(int pidType)
{
    switch (pidType) {
    case OBJ_TYPE_CRITTER:
        return "critter";
    case OBJ_TYPE_ITEM:
        return "item";
    case OBJ_TYPE_SCENERY:
        return "scenery";
    case OBJ_TYPE_WALL:
        return "wall";
    case OBJ_TYPE_TILE:
        return "tile";
    case OBJ_TYPE_MISC:
        return "misc";
    default:
        return "unknown";
    }
}

Object* cliFindWorldObjectById(int objectId)
{
    for (Object* object = obj_find_first(); object != NULL; object = obj_find_next()) {
        if (object->id == objectId) {
            return object;
        }
    }

    return NULL;
}

Object* cliFindInventoryObjectById(Object* owner, int objectId)
{
    if (owner == NULL) {
        return NULL;
    }

    Inventory* inventory = &(owner->data.inventory);
    for (int index = 0; index < inventory->length; index++) {
        InventoryItem* inventoryItem = &(inventory->items[index]);
        Object* item = inventoryItem->item;

        if (item->id == objectId) {
            return item;
        }

        if (item_get_type(item) == ITEM_TYPE_CONTAINER) {
            Object* nested = cliFindInventoryObjectById(item, objectId);
            if (nested != NULL) {
                return nested;
            }
        }
    }

    return NULL;
}

Object* cliFindAnyObjectById(int objectId)
{
    Object* object = cliFindWorldObjectById(objectId);
    if (object != NULL) {
        return object;
    }

    return cliFindInventoryObjectById(obj_dude, objectId);
}

Object* cliFindPlayerItemById(int objectId)
{
    if (obj_dude == NULL) {
        return NULL;
    }

    return inven_find_id(obj_dude, objectId);
}

bool cliIsExitGrid(Object* object)
{
    if (object == NULL) {
        return false;
    }

    if (PID_TYPE(object->pid) != OBJ_TYPE_MISC) {
        return false;
    }

    return object->pid >= PROTO_ID_0x5000010 && object->pid <= PROTO_ID_0x5000017;
}

std::vector<Object*> cliCollectObjectsAtElevation(int elevation)
{
    std::vector<Object*> objects;
    for (Object* object = obj_find_first_at(elevation); object != NULL; object = obj_find_next_at()) {
        if ((object->flags & OBJECT_HIDDEN) != 0) {
            continue;
        }

        objects.push_back(object);
    }

    return objects;
}

Object* cliFindNearestExitGrid(int maxDistance)
{
    if (obj_dude == NULL) {
        return NULL;
    }

    Object* nearest = NULL;
    int nearestDistance = INT_MAX;
    for (Object* object = obj_find_first_at(obj_dude->elevation); object != NULL; object = obj_find_next_at()) {
        if (!cliIsExitGrid(object)) {
            continue;
        }

        int distance = tile_dist(obj_dude->tile, object->tile);
        if (distance > maxDistance) {
            continue;
        }

        if (nearest == NULL || distance < nearestDistance || (distance == nearestDistance && object->id < nearest->id)) {
            nearest = object;
            nearestDistance = distance;
        }
    }

    return nearest;
}

Object* cliPathTrackingCallback(Object* object, int tile, int elevation)
{
    Object* blocker = obj_blocking_at(object, tile, elevation);

    if (gCliPathTrackingContext.active && blocker == NULL) {
        int distance = tile_dist(tile, gCliPathTrackingContext.targetTile);
        if (gCliPathTrackingContext.bestTile == -1
            || distance < gCliPathTrackingContext.bestDistance
            || (distance == gCliPathTrackingContext.bestDistance && tile < gCliPathTrackingContext.bestTile)) {
            gCliPathTrackingContext.bestTile = tile;
            gCliPathTrackingContext.bestDistance = distance;
        }
    }

    return blocker;
}

int cliMakePathWithClosest(Object* object, int from, int to, unsigned char* rotations, int flags, int* closestTilePtr)
{
    gCliPathTrackingContext.active = true;
    gCliPathTrackingContext.targetTile = to;
    gCliPathTrackingContext.bestTile = -1;
    gCliPathTrackingContext.bestDistance = INT_MAX;

    int pathLength = make_path_func(object, from, to, rotations, flags, cliPathTrackingCallback);

    int closestTile = gCliPathTrackingContext.bestTile;

    gCliPathTrackingContext.active = false;
    gCliPathTrackingContext.targetTile = -1;
    gCliPathTrackingContext.bestTile = -1;
    gCliPathTrackingContext.bestDistance = INT_MAX;

    if (closestTilePtr != NULL) {
        if (closestTile == -1) {
            closestTile = from;
        }
        *closestTilePtr = closestTile;
    }

    return pathLength;
}

int cliAdvanceAlongPath(int startTile, const unsigned char* rotations, int steps)
{
    int tile = startTile;
    for (int index = 0; index < steps; index++) {
        tile = tile_num_in_direction(tile, rotations[index], 1);
    }
    return tile;
}

int cliValidateClosestTile(Object* object, int fromTile, int closestTile)
{
    if (object == NULL) {
        return fromTile;
    }

    if (make_path(object, fromTile, closestTile, NULL, 0) == 0) {
        return fromTile;
    }

    return closestTile;
}

bool cliPlanFallbackMove(Object* object, int fromTile, int toTile, int* destinationTilePtr, int* plannedStepsPtr, bool* cappedPtr)
{
    if (destinationTilePtr == NULL || plannedStepsPtr == NULL || cappedPtr == NULL) {
        return false;
    }

    *destinationTilePtr = fromTile;
    *plannedStepsPtr = 0;
    *cappedPtr = false;

    if (object == NULL || fromTile == toTile) {
        return false;
    }

    unsigned char rotations[kCliGotoPathRotationsCapacity];
    int pathLength = make_path(object, fromTile, toTile, rotations, 0);
    if (pathLength <= 0) {
        return false;
    }

    *plannedStepsPtr = std::min(pathLength, kCliGotoMaxPathLength);
    *cappedPtr = *plannedStepsPtr < pathLength;
    *destinationTilePtr = *cappedPtr ? cliAdvanceAlongPath(fromTile, rotations, *plannedStepsPtr) : toTile;
    return *plannedStepsPtr > 0;
}

bool cliWaitForObjectAnimationToComplete(Object* object, unsigned int timeoutMs)
{
    if (object == NULL) {
        return false;
    }

    unsigned int start = get_time();
    while (anim_busy(object) != 0) {
        object_animate();

        if (elapsed_tocks(get_time(), start) > timeoutMs) {
            return false;
        }

        pause_for_tocks(kCliGotoWaitStepMs);
    }

    return true;
}

int cliGetPerceptionRange()
{
    if (obj_dude == NULL) {
        return 0;
    }

    int perception = stat_level(obj_dude, STAT_PERCEPTION);
    return std::max(6, perception * 3);
}

bool cliIsContainerForLook(Object* object)
{
    if (object == NULL) {
        return false;
    }

    Proto* proto;
    if (proto_ptr(object->pid, &proto) == -1) {
        return object->data.inventory.length > 0;
    }

    if (PID_TYPE(object->pid) == OBJ_TYPE_ITEM) {
        if (proto->item.type == ITEM_TYPE_CONTAINER) {
            return true;
        }

        return object->data.inventory.length > 0;
    }

    if (PID_TYPE(object->pid) == OBJ_TYPE_SCENERY && proto->scenery.type != SCENERY_TYPE_DOOR) {
        return object->data.inventory.length > 0;
    }

    return false;
}

bool cliIsNotableSceneryForLook(Object* object)
{
    if (object == NULL || PID_TYPE(object->pid) != OBJ_TYPE_SCENERY) {
        return false;
    }

    Proto* proto;
    if (proto_ptr(object->pid, &proto) == -1) {
        return false;
    }

    if (proto->scenery.type == SCENERY_TYPE_DOOR) {
        return false;
    }

    if (proto->scenery.type == SCENERY_TYPE_ELEVATOR
        || proto->scenery.type == SCENERY_TYPE_LADDER_UP
        || proto->scenery.type == SCENERY_TYPE_LADDER_DOWN) {
        return true;
    }

    const char* rawName = object_name(object);
    std::string lowerName = cliToLower(rawName != NULL ? rawName : "");

    const char* excludedKeywords[] = {
        "wall",
        "blocker",
        "secret block",
        "cave wall",
        "pipe",
        "vent",
        "light",
    };

    for (const char* keyword : excludedKeywords) {
        if (lowerName.find(keyword) != std::string::npos) {
            return false;
        }
    }

    const char* includedKeywords[] = {
        "computer",
        "terminal",
        "elevator",
        "ladder",
        "bed",
        "locker",
        "desk",
        "console",
        "panel",
    };

    for (const char* keyword : includedKeywords) {
        if (lowerName.find(keyword) != std::string::npos) {
            return true;
        }
    }

    return false;
}

void cliSortNearbyObjects(std::vector<NearbyObjectInfo>* entries)
{
    if (entries == NULL) {
        return;
    }

    std::sort(entries->begin(), entries->end(), [](const NearbyObjectInfo& lhs, const NearbyObjectInfo& rhs) {
        if (lhs.distance != rhs.distance) {
            return lhs.distance < rhs.distance;
        }

        return lhs.object->id < rhs.object->id;
    });
}

int cliParseSkill(const std::string& skill)
{
    std::string normalized = cliNormalizeName(skill);

    for (int index = 0; index < SKILL_COUNT; index++) {
        const char* skillName = skill_name(index);
        if (skillName == NULL) {
            continue;
        }

        if (normalized == cliNormalizeName(skillName)) {
            return index;
        }
    }

    return -1;
}

int cliParseTrait(const std::string& trait)
{
    int value;
    if (cliParseInteger(trait, &value)) {
        if (value >= 1 && value <= TRAIT_COUNT) {
            return value - 1;
        }

        if (value >= 0 && value < TRAIT_COUNT) {
            return value;
        }
    }

    std::string normalized = cliNormalizeName(trait);

    for (int index = 0; index < TRAIT_COUNT; index++) {
        const char* traitName = trait_name(index);
        if (traitName == NULL) {
            continue;
        }

        if (normalized == cliNormalizeName(traitName)) {
            return index;
        }
    }

    return -1;
}

int cliParseSpecialStat(const std::string& stat)
{
    std::string normalized = cliNormalizeName(stat);

    if (normalized == "str" || normalized == "strength") {
        return STAT_STRENGTH;
    }

    if (normalized == "per" || normalized == "perception") {
        return STAT_PERCEPTION;
    }

    if (normalized == "end" || normalized == "endurance") {
        return STAT_ENDURANCE;
    }

    if (normalized == "cha" || normalized == "charisma") {
        return STAT_CHARISMA;
    }

    if (normalized == "int" || normalized == "intelligence") {
        return STAT_INTELLIGENCE;
    }

    if (normalized == "agi" || normalized == "agility") {
        return STAT_AGILITY;
    }

    if (normalized == "luk" || normalized == "luck") {
        return STAT_LUCK;
    }

    return -1;
}

bool cliHasTaggedSkill(int skill)
{
    for (int index = 0; index < NUM_TAGGED_SKILLS; index++) {
        if (editor_get_temp_tag_skill(index) == skill) {
            return true;
        }
    }

    return false;
}

bool cliHasSelectedTrait(int trait)
{
    for (int index = 0; index < 2; index++) {
        if (editor_get_temp_trait(index) == trait) {
            return true;
        }
    }

    return false;
}

int cliParseHitLocation(const std::string& location)
{
    std::string normalized = cliNormalizeName(location);

    if (normalized == "head") {
        return HIT_LOCATION_HEAD;
    }

    if (normalized == "leftarm" || normalized == "larm") {
        return HIT_LOCATION_LEFT_ARM;
    }

    if (normalized == "rightarm" || normalized == "rarm") {
        return HIT_LOCATION_RIGHT_ARM;
    }

    if (normalized == "torso" || normalized == "body") {
        return HIT_LOCATION_TORSO;
    }

    if (normalized == "rightleg" || normalized == "rleg") {
        return HIT_LOCATION_RIGHT_LEG;
    }

    if (normalized == "leftleg" || normalized == "lleg") {
        return HIT_LOCATION_LEFT_LEG;
    }

    if (normalized == "eyes" || normalized == "eye") {
        return HIT_LOCATION_EYES;
    }

    if (normalized == "groin") {
        return HIT_LOCATION_GROIN;
    }

    return -1;
}

int cliParseKeyCode(const std::string& value)
{
    int keyCode;
    if (cliParseInteger(value, &keyCode)) {
        return keyCode;
    }

    std::string lowered = cliToLower(value);
    if (lowered == "enter" || lowered == "return") {
        return KEY_RETURN;
    }

    if (lowered == "esc" || lowered == "escape") {
        return KEY_ESCAPE;
    }

    if (lowered == "space") {
        return KEY_SPACE;
    }

    if (lowered == "tab") {
        return KEY_TAB;
    }

    if (lowered == "up") {
        return KEY_ARROW_UP;
    }

    if (lowered == "down") {
        return KEY_ARROW_DOWN;
    }

    if (lowered == "left") {
        return KEY_ARROW_LEFT;
    }

    if (lowered == "right") {
        return KEY_ARROW_RIGHT;
    }

    if (lowered == "home") {
        return KEY_HOME;
    }

    if (lowered == "end") {
        return KEY_END;
    }

    if (lowered == "pgup" || lowered == "pageup") {
        return KEY_PAGE_UP;
    }

    if (lowered == "pgdown" || lowered == "pagedown") {
        return KEY_PAGE_DOWN;
    }

    if (value.size() == 1) {
        return static_cast<unsigned char>(value[0]);
    }

    return -1;
}

std::string cliGetMode()
{
    if (in_main_menu) {
        return "mainmenu";
    }

    if (editor_is_active()) {
        if (editor_is_creation_mode()) {
            return "chargen";
        }

        return "character";
    }

    if (worldmap_is_active()) {
        return "worldmap";
    }

    if (dialog_active()) {
        return "dialogue";
    }

    if (pipboy_is_open()) {
        return "pipboy";
    }

    if (inven_is_open()) {
        return "inventory";
    }

    if (isInCombat()) {
        return "combat";
    }

    return "exploration";
}

std::string cliStripDialogPrefix(const char* text)
{
    if (text == NULL) {
        return std::string();
    }

    while (*text != '\0') {
        unsigned char ch = static_cast<unsigned char>(*text);
        if (isspace(ch) == 0 && ch != 0x95) {
            break;
        }
        text++;
    }

    return std::string(text);
}

std::string cliBuildInventoryDump()
{
    std::ostringstream out;
    out << "[INVENTORY]\n";

    if (obj_dude == NULL) {
        out << "count=0\n";
        return out.str();
    }

    Inventory* inventory = &(obj_dude->data.inventory);
    out << "count=" << inventory->length << "\n";

    for (int index = 0; index < inventory->length; index++) {
        InventoryItem* inventoryItem = &(inventory->items[index]);
        Object* item = inventoryItem->item;

        out << '[' << item->id << "] ";
        out << "name=" << cliEscapeValue(item_name(item));
        out << " quantity=" << inventoryItem->quantity;

        std::vector<std::string> equippedSlots;
        if ((item->flags & OBJECT_IN_LEFT_HAND) != 0) {
            equippedSlots.push_back("left_hand");
        }
        if ((item->flags & OBJECT_IN_RIGHT_HAND) != 0) {
            equippedSlots.push_back("right_hand");
        }
        if ((item->flags & OBJECT_WORN) != 0) {
            equippedSlots.push_back("armor");
        }

        if (!equippedSlots.empty()) {
            out << " equipped=";
            for (size_t slotIndex = 0; slotIndex < equippedSlots.size(); slotIndex++) {
                if (slotIndex != 0) {
                    out << ',';
                }
                out << equippedSlots[slotIndex];
            }
        }

        out << "\n";
    }

    return out.str();
}

std::string cliBuildStateDump()
{
    std::ostringstream out;

    out << "[MODE]\n";
    out << "mode=" << cliGetMode() << "\n";
    out << "game_state=" << game_state() << "\n";
    out << "map=" << map_data.name << "\n";

    out << "\n[PLAYER]\n";
    if (obj_dude == NULL) {
        out << "present=0\n";
    } else {
        int maxHp = stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS);
        int maxAp = stat_level(obj_dude, STAT_MAXIMUM_ACTION_POINTS);
        int currentAp = isInCombat() ? obj_dude->data.critter.combat.ap : maxAp;

        out << "name=" << cliEscapeValue(critter_name(obj_dude)) << "\n";
        out << "tile=" << obj_dude->tile << "\n";
        out << "elevation=" << obj_dude->elevation << "\n";
        out << "hp=" << critter_get_hits(obj_dude) << '/' << maxHp << "\n";
        out << "ap=" << currentAp << '/' << maxAp << "\n";
        out << "strength=" << stat_level(obj_dude, STAT_STRENGTH) << "\n";
        out << "perception=" << stat_level(obj_dude, STAT_PERCEPTION) << "\n";
        out << "endurance=" << stat_level(obj_dude, STAT_ENDURANCE) << "\n";
        out << "charisma=" << stat_level(obj_dude, STAT_CHARISMA) << "\n";
        out << "intelligence=" << stat_level(obj_dude, STAT_INTELLIGENCE) << "\n";
        out << "agility=" << stat_level(obj_dude, STAT_AGILITY) << "\n";
        out << "luck=" << stat_level(obj_dude, STAT_LUCK) << "\n";
        out << "ac=" << stat_level(obj_dude, STAT_ARMOR_CLASS) << "\n";
        out << "xp=" << stat_pc_get(PC_STAT_EXPERIENCE) << "\n";
        out << "level=" << stat_pc_get(PC_STAT_LEVEL) << "\n";
    }

    out << "\n[EQUIPMENT]\n";
    if (obj_dude == NULL) {
        out << "left_hand=none\n";
        out << "right_hand=none\n";
        out << "armor=none\n";
    } else {
        Object* left = inven_left_hand(obj_dude);
        Object* right = inven_right_hand(obj_dude);
        Object* armor = inven_worn(obj_dude);

        if (left != NULL) {
            out << "left_hand=[" << left->id << "] " << cliEscapeValue(item_name(left)) << "\n";
        } else {
            out << "left_hand=none\n";
        }

        if (right != NULL) {
            out << "right_hand=[" << right->id << "] " << cliEscapeValue(item_name(right)) << "\n";
        } else {
            out << "right_hand=none\n";
        }

        if (armor != NULL) {
            out << "armor=[" << armor->id << "] " << cliEscapeValue(item_name(armor)) << "\n";
        } else {
            out << "armor=none\n";
        }
    }

    out << "\n" << cliBuildInventoryDump();

    out << "\n[SURROUNDINGS]\n";
    if (obj_dude == NULL) {
        out << "count=0\n";
    } else {
        int perception = stat_level(obj_dude, STAT_PERCEPTION);
        int maxDistance = std::max(6, perception * 3);

        std::vector<Object*> objects = cliCollectObjectsAtElevation(obj_dude->elevation);

        std::vector<NearbyObjectInfo> nearby;
        nearby.reserve(objects.size());

        for (Object* object : objects) {
            if (object == obj_dude) {
                continue;
            }

            if ((object->flags & OBJECT_HIDDEN) != 0) {
                continue;
            }

            int distance = tile_dist(obj_dude->tile, object->tile);
            if (distance > maxDistance) {
                continue;
            }

            if (distance > 0 && !can_see(obj_dude, object)) {
                continue;
            }

            NearbyObjectInfo info;
            info.object = object;
            info.distance = distance;
            info.direction = distance == 0 ? -1 : tile_dir(obj_dude->tile, object->tile);
            nearby.push_back(info);
        }

        std::sort(nearby.begin(), nearby.end(), [](const NearbyObjectInfo& lhs, const NearbyObjectInfo& rhs) {
            if (lhs.distance != rhs.distance) {
                return lhs.distance < rhs.distance;
            }

            return lhs.object->id < rhs.object->id;
        });

        out << "range=" << maxDistance << "\n";
        out << "count=" << nearby.size() << "\n";

        for (const NearbyObjectInfo& info : nearby) {
            Object* object = info.object;

            out << '[' << object->id << "] ";
            out << "name=" << cliEscapeValue(object_name(object));
            out << " type=" << cliObjectTypeToString(object);
            out << " distance=" << info.distance;

            if (info.direction >= 0) {
                out << " direction=" << cliDirectionToString(info.direction);
            } else {
                out << " direction=here";
            }

            if (PID_TYPE(object->pid) == OBJ_TYPE_CRITTER) {
                int maxHp = stat_level(object, STAT_MAXIMUM_HIT_POINTS);
                int hostile = object->data.critter.combat.team != obj_dude->data.critter.combat.team ? 1 : 0;
                out << " hp=" << critter_get_hits(object) << '/' << maxHp;
                out << " hostile=" << hostile;
            }

            out << "\n";
        }
    }

    if (dialog_active()) {
        out << "\n[DIALOGUE]\n";

        if (dialog_target != NULL) {
            out << "npc=" << cliEscapeValue(object_name(dialog_target)) << "\n";
            out << "npc_id=" << dialog_target->id << "\n";
        } else {
            out << "npc=none\n";
        }

        const char* reply = gdialog_get_reply_text();
        out << "reply=" << cliEscapeValue(reply != NULL ? reply : "") << "\n";

        int optionCount = gdialog_get_option_count();
        out << "option_count=" << optionCount << "\n";
        for (int index = 0; index < optionCount; index++) {
            const char* optionText = gdialog_get_option_text(index);
            out << (index + 1) << "=" << cliEscapeValue(cliStripDialogPrefix(optionText)) << "\n";
        }
    }

    if (isInCombat()) {
        out << "\n[COMBAT]\n";

        Object* whoseTurn = combat_whose_turn();
        if (whoseTurn != NULL) {
            out << "turn=[" << whoseTurn->id << "] " << cliEscapeValue(object_name(whoseTurn)) << "\n";
        } else {
            out << "turn=none\n";
        }

        if (obj_dude != NULL) {
            out << "remaining_ap=" << obj_dude->data.critter.combat.ap << "\n";
        }

        std::vector<Object*> objects = cliCollectObjectsAtElevation(map_elevation);

        std::vector<NearbyObjectInfo> enemies;
        for (Object* critter : objects) {
            if (PID_TYPE(critter->pid) != OBJ_TYPE_CRITTER) {
                continue;
            }

            if (critter == obj_dude) {
                continue;
            }

            if ((critter->data.critter.combat.results & DAM_DEAD) != 0) {
                continue;
            }

            if (critter->data.critter.combat.team == obj_dude->data.critter.combat.team) {
                continue;
            }

            NearbyObjectInfo info;
            info.object = critter;
            info.distance = tile_dist(obj_dude->tile, critter->tile);
            info.direction = tile_dir(obj_dude->tile, critter->tile);
            enemies.push_back(info);
        }

        std::sort(enemies.begin(), enemies.end(), [](const NearbyObjectInfo& lhs, const NearbyObjectInfo& rhs) {
            if (lhs.distance != rhs.distance) {
                return lhs.distance < rhs.distance;
            }

            return lhs.object->id < rhs.object->id;
        });

        out << "enemy_count=" << enemies.size() << "\n";
        for (const NearbyObjectInfo& enemy : enemies) {
            Object* critter = enemy.object;
            out << '[' << critter->id << "] ";
            out << "name=" << cliEscapeValue(object_name(critter));
            out << " hp=" << critter_get_hits(critter) << '/' << stat_level(critter, STAT_MAXIMUM_HIT_POINTS);
            out << " distance=" << enemy.distance;
            out << " direction=" << cliDirectionToString(enemy.direction);
            out << "\n";
        }
    }

    if (worldmap_is_active()) {
        out << "\n[WORLDMAP]\n";

        int worldX;
        int worldY;
        worldmap_get_position(&worldX, &worldY);
        out << "position=" << worldX << ',' << worldY << "\n";

        int known[TOWN_COUNT];
        int knownCount = worldmap_get_known_towns(known, TOWN_COUNT);
        out << "known_count=" << knownCount << "\n";

        for (int index = 0; index < knownCount && index < TOWN_COUNT; index++) {
            const char* townName = worldmap_get_town_name(known[index]);
            out << '[' << known[index] << "] " << cliEscapeValue(townName != NULL ? townName : "") << "\n";
        }
    }

    out << "\n[DISPLAY_LOG]\n";
    char displayLog[4096];
    int messageCount = display_get_last_messages(displayLog, sizeof(displayLog), kMaxDisplayLogLines);
    out << "lines=" << messageCount << "\n";

    if (messageCount > 0) {
        std::istringstream lines(displayLog);
        std::string line;
        int lineIndex = 1;
        while (std::getline(lines, line)) {
            out << lineIndex << '=' << cliEscapeValue(line) << "\n";
            lineIndex++;
        }
    }

    return out.str();
}

std::string cliBuildLookDump()
{
    std::ostringstream out;

    std::vector<NearbyObjectInfo> npcs;
    std::vector<NearbyObjectInfo> items;
    std::vector<NearbyObjectInfo> containers;
    std::vector<NearbyObjectInfo> doors;
    std::vector<NearbyObjectInfo> exits;
    std::vector<NearbyObjectInfo> scenery;

    if (obj_dude != NULL) {
        int maxDistance = cliGetPerceptionRange();
        std::vector<Object*> objects = cliCollectObjectsAtElevation(obj_dude->elevation);

        for (Object* object : objects) {
            if (object == obj_dude) {
                continue;
            }

            if ((object->flags & OBJECT_HIDDEN) != 0) {
                continue;
            }

            int distance = tile_dist(obj_dude->tile, object->tile);
            if (distance > maxDistance) {
                continue;
            }

            if (distance > 0 && !can_see(obj_dude, object)) {
                continue;
            }

            NearbyObjectInfo info;
            info.object = object;
            info.distance = distance;
            info.direction = distance == 0 ? -1 : tile_dir(obj_dude->tile, object->tile);

            Proto* proto = NULL;
            proto_ptr(object->pid, &proto);

            int pidType = PID_TYPE(object->pid);
            if (pidType == OBJ_TYPE_CRITTER) {
                npcs.push_back(info);
                continue;
            }

            if (cliIsExitGrid(object)) {
                exits.push_back(info);
                continue;
            }

            if (pidType == OBJ_TYPE_SCENERY && proto != NULL && proto->scenery.type == SCENERY_TYPE_DOOR) {
                doors.push_back(info);
                continue;
            }

            if (cliIsContainerForLook(object)) {
                containers.push_back(info);
                continue;
            }

            if (pidType == OBJ_TYPE_ITEM) {
                items.push_back(info);
                continue;
            }

            if (pidType == OBJ_TYPE_SCENERY && cliIsNotableSceneryForLook(object)) {
                scenery.push_back(info);
            }
        }
    }

    cliSortNearbyObjects(&npcs);
    cliSortNearbyObjects(&items);
    cliSortNearbyObjects(&containers);
    cliSortNearbyObjects(&doors);
    cliSortNearbyObjects(&exits);
    cliSortNearbyObjects(&scenery);

    auto appendDirection = [&](std::ostringstream& stream, int direction) {
        if (direction >= 0) {
            stream << cliDirectionToString(direction);
        } else {
            stream << "here";
        }
    };

    out << "[NPCS]\n";
    out << "count=" << npcs.size() << "\n";
    if (obj_dude != NULL) {
        for (const NearbyObjectInfo& entry : npcs) {
            Object* object = entry.object;
            int maxHp = stat_level(object, STAT_MAXIMUM_HIT_POINTS);
            int hostile = object->data.critter.combat.team != obj_dude->data.critter.combat.team ? 1 : 0;
            out << '[' << object->id << "] ";
            out << "name=" << cliEscapeValue(object_name(object));
            out << " distance=" << entry.distance;
            out << " direction=";
            appendDirection(out, entry.direction);
            out << " tile=" << object->tile;
            out << " hp=" << critter_get_hits(object) << '/' << maxHp;
            out << " hostile=" << hostile;
            out << "\n";
        }
    }

    out << "\n[ITEMS]\n";
    out << "count=" << items.size() << "\n";
    for (const NearbyObjectInfo& entry : items) {
        Object* object = entry.object;
        out << '[' << object->id << "] ";
        out << "name=" << cliEscapeValue(object_name(object));
        out << " distance=" << entry.distance;
        out << " direction=";
        appendDirection(out, entry.direction);
        out << " tile=" << object->tile;
        out << "\n";
    }

    out << "\n[CONTAINERS]\n";
    out << "count=" << containers.size() << "\n";
    for (const NearbyObjectInfo& entry : containers) {
        Object* object = entry.object;
        out << '[' << object->id << "] ";
        out << "name=" << cliEscapeValue(object_name(object));
        out << " distance=" << entry.distance;
        out << " direction=";
        appendDirection(out, entry.direction);
        out << " tile=" << object->tile;
        out << "\n";
    }

    out << "\n[DOORS]\n";
    out << "count=" << doors.size() << "\n";
    for (const NearbyObjectInfo& entry : doors) {
        Object* object = entry.object;
        const char* state;
        if (obj_is_locked(object)) {
            state = "locked";
        } else if (obj_is_open(object) != 0) {
            state = "open";
        } else {
            state = "closed";
        }

        out << '[' << object->id << "] ";
        out << "name=" << cliEscapeValue(object_name(object));
        out << " distance=" << entry.distance;
        out << " direction=";
        appendDirection(out, entry.direction);
        out << " tile=" << object->tile;
        out << " state=" << state;
        out << "\n";
    }

    out << "\n[EXITS]\n";
    out << "count=" << exits.size() << "\n";
    for (const NearbyObjectInfo& entry : exits) {
        Object* object = entry.object;
        out << '[' << object->id << "] ";
        out << "name=" << cliEscapeValue(object_name(object));
        out << " distance=" << entry.distance;
        out << " direction=";
        appendDirection(out, entry.direction);
        out << " tile=" << object->tile;
        out << " pid=0x" << std::hex << object->pid << std::dec;
        out << "\n";
    }

    out << "\n[SCENERY]\n";
    out << "count=" << scenery.size() << "\n";
    for (const NearbyObjectInfo& entry : scenery) {
        Object* object = entry.object;
        out << '[' << object->id << "] ";
        out << "name=" << cliEscapeValue(object_name(object));
        out << " distance=" << entry.distance;
        out << " direction=";
        appendDirection(out, entry.direction);
        out << " tile=" << object->tile;
        out << "\n";
    }

    return out.str();
}

CliCommandResponse cliOk(const std::string& body)
{
    CliCommandResponse response;
    response.ok = true;
    response.body = body;
    return response;
}

CliCommandResponse cliError(const std::string& body)
{
    CliCommandResponse response;
    response.ok = false;
    response.body = body;
    return response;
}

CliCommandResponse cliQueueKey(int keyCode)
{
    GNW_add_input_buffer(keyCode);

    std::ostringstream out;
    out << "queued_key=" << keyCode;
    return cliOk(out.str());
}

std::string cliHelpText()
{
    return std::string(
        "Commands:\n"
        "state | look | help | debug_objects | debug_nearby\n"
        "new_game | load_game | exit\n"
        "key <code|name>\n"
        "stat_inc <stat> | stat_dec <stat>\n"
        "tag_skill <skill_name> | trait_select <trait_name_or_index> | set_name <name> | done\n"
        "move <direction> | move_to <tile> | goto <object_id_or_tile> | enter | scan_exits\n"
        "interact <object_id> | talk <npc_id> | pickup <object_id>\n"
        "use_skill <skill_name> <target_id> | wait <hours>\n"
        "attack <target_id> [body_part] | end_turn | reload | change_weapon | flee\n"
        "say <option_number> | barter | end\n"
        "inventory | equip <item_id> <slot> | unequip <slot> | use <item_id> | drop <item_id> | examine <item_id>\n"
        "worldmap | travel <location_name> | cancel\n"
        "save <slot> | pipboy | character | automap | sneak");
}

CliCommandResponse cliExecuteCommand(const std::string& line)
{
    std::vector<std::string> tokens = cliTokenize(line);
    if (tokens.empty()) {
        return cliError("empty_command");
    }

    std::string command = cliToLower(tokens[0]);

    if (command == "help") {
        return cliOk(cliHelpText());
    }

    if (command == "state") {
        return cliOk(cliBuildStateDump());
    }

    if (command == "look") {
        if (obj_dude == NULL) {
            return cliError("player_unavailable");
        }

        return cliOk(cliBuildLookDump());
    }

    if (command == "debug_objects") {
        std::ostringstream out;
        out << "player_tile=" << (obj_dude != NULL ? obj_dude->tile : -1) << "\n";
        out << "player_elevation=" << (obj_dude != NULL ? obj_dude->elevation : -1);

        for (int elevation = 0; elevation <= 2; elevation++) {
            std::vector<Object*> objects = cliCollectObjectsAtElevation(elevation);
            int objectCount = static_cast<int>(objects.size());

            out << "\n\n[elevation " << elevation << "]\n";

            int critterCount = 0;
            int itemCount = 0;
            int sceneryCount = 0;
            int wallCount = 0;
            int tileCount = 0;
            int miscCount = 0;
            int shownCount = 0;

            for (Object* object : objects) {
                int pidType = PID_TYPE(object->pid);
                switch (pidType) {
                case OBJ_TYPE_CRITTER:
                    critterCount++;
                    break;
                case OBJ_TYPE_ITEM:
                    itemCount++;
                    break;
                case OBJ_TYPE_SCENERY:
                    sceneryCount++;
                    break;
                case OBJ_TYPE_WALL:
                    wallCount++;
                    break;
                case OBJ_TYPE_TILE:
                    tileCount++;
                    break;
                case OBJ_TYPE_MISC:
                    miscCount++;
                    break;
                }

                if (shownCount >= kCliDebugObjectsPerElevationLimit) {
                    continue;
                }

                const char* name = object_name(object);
                int distance = obj_dude != NULL ? tile_dist(obj_dude->tile, object->tile) : -1;

                out << '[' << object->id << "] ";
                out << "pid=0x" << std::hex << object->pid << std::dec;
                out << " type=" << cliPidTypeToString(pidType);
                out << " name=" << cliEscapeValue(name != NULL ? name : "");
                out << " tile=" << object->tile;
                out << " elevation=" << object->elevation;
                out << " flags=0x" << std::hex << object->flags << std::dec;
                out << " distance=" << distance;
                out << "\n";

                shownCount++;
            }

            out << "count=" << objectCount << "\n";
            out << "shown=" << shownCount << "\n";
            out << "type_critter=" << critterCount << "\n";
            out << "type_item=" << itemCount << "\n";
            out << "type_scenery=" << sceneryCount << "\n";
            out << "type_wall=" << wallCount << "\n";
            out << "type_tile=" << tileCount << "\n";
            out << "type_misc=" << miscCount << "\n";
            out << "truncated=" << (objectCount > shownCount ? objectCount - shownCount : 0);
        }

        return cliOk(out.str());
    }

    if (command == "debug_nearby") {
        if (obj_dude == NULL) {
            return cliError("player_unavailable");
        }

        constexpr int kDebugNearbyRange = 999;

        std::vector<Object*> objects = cliCollectObjectsAtElevation(obj_dude->elevation);
        int objectCount = static_cast<int>(objects.size());

        std::vector<NearbyObjectInfo> nearby;
        nearby.reserve(objectCount);

        for (Object* object : objects) {
            if (object == obj_dude) {
                continue;
            }

            int distance = tile_dist(obj_dude->tile, object->tile);
            if (distance > kDebugNearbyRange) {
                continue;
            }

            NearbyObjectInfo info;
            info.object = object;
            info.distance = distance;
            info.direction = distance == 0 ? -1 : tile_dir(obj_dude->tile, object->tile);
            nearby.push_back(info);
        }

        std::sort(nearby.begin(), nearby.end(), [](const NearbyObjectInfo& lhs, const NearbyObjectInfo& rhs) {
            if (lhs.distance != rhs.distance) {
                return lhs.distance < rhs.distance;
            }

            return lhs.object->id < rhs.object->id;
        });

        std::ostringstream out;
        out << "range=" << kDebugNearbyRange << "\n";
        out << "count=" << nearby.size() << "\n";

        for (const NearbyObjectInfo& info : nearby) {
            Object* object = info.object;

            out << '[' << object->id << "] ";
            out << "name=" << cliEscapeValue(object_name(object));
            out << " type=" << cliObjectTypeToString(object);
            out << " distance=" << info.distance;

            if (info.direction >= 0) {
                out << " direction=" << cliDirectionToString(info.direction);
            } else {
                out << " direction=here";
            }

            if (PID_TYPE(object->pid) == OBJ_TYPE_CRITTER) {
                int maxHp = stat_level(object, STAT_MAXIMUM_HIT_POINTS);
                int hostile = object->data.critter.combat.team != obj_dude->data.critter.combat.team ? 1 : 0;
                out << " hp=" << critter_get_hits(object) << '/' << maxHp;
                out << " hostile=" << hostile;
            }

            out << "\n";
        }

        return cliOk(out.str());
    }

    if (command == "new_game") {
        if (!in_main_menu) {
            return cliError("new_game_available_only_in_main_menu");
        }

        return cliQueueKey(KEY_LOWERCASE_N);
    }

    if (command == "load_game") {
        if (!in_main_menu) {
            return cliError("load_game_available_only_in_main_menu");
        }

        return cliQueueKey(KEY_LOWERCASE_L);
    }

    if (command == "exit") {
        if (in_main_menu) {
            GNW_add_input_buffer(KEY_LOWERCASE_E);
        } else {
            game_user_wants_to_quit = 3;
        }

        return cliOk("quit_requested=1");
    }

    if (command == "key") {
        if (tokens.size() < 2) {
            return cliError("usage=key <code|name>");
        }

        int keyCode = cliParseKeyCode(tokens[1]);
        if (keyCode < 0) {
            return cliError("invalid_key");
        }

        return cliQueueKey(keyCode);
    }

    if (command == "stat_inc" || command == "stat_dec") {
        if (tokens.size() < 2) {
            return cliError("usage=stat_inc <stat> or stat_dec <stat>");
        }

        if (!editor_is_creation_mode()) {
            return cliError("stat_changes_available_only_in_chargen");
        }

        if (obj_dude == NULL) {
            return cliError("player_unavailable");
        }

        int stat = cliParseSpecialStat(tokens[1]);
        if (stat < STAT_STRENGTH || stat > STAT_LUCK) {
            return cliError("invalid_special_stat");
        }

        int rc;
        if (command == "stat_inc") {
            if (character_points <= 0) {
                return cliError("no_character_points_remaining");
            }

            rc = inc_stat(obj_dude, stat);
            if (rc != 0) {
                return cliError("stat_increase_failed");
            }

            character_points--;
        } else {
            rc = dec_stat(obj_dude, stat);
            if (rc != 0) {
                return cliError("stat_decrease_failed");
            }

            character_points++;
        }

        stat_recalc_derived(obj_dude);

        std::ostringstream out;
        out << "stat=" << cliEscapeValue(stat_name(stat)) << "\n";
        out << "value=" << stat_level(obj_dude, stat) << "\n";
        out << "remaining_points=" << character_points;
        return cliOk(out.str());
    }

    if (command == "tag_skill") {
        if (tokens.size() < 2) {
            return cliError("usage=tag_skill <skill_name>");
        }

        if (!editor_is_creation_mode()) {
            return cliError("tag_skill_available_only_in_chargen");
        }

        int skill = cliParseSkill(cliJoinTokens(tokens, 1));
        if (skill < 0) {
            return cliError("invalid_skill");
        }

        bool wasTagged = cliHasTaggedSkill(skill);
        if (!wasTagged && editor_get_remaining_tag_skill_count() <= 0) {
            return cliError("no_tag_skill_slots_remaining");
        }

        if (editor_cli_tag_skill(skill) != 0) {
            return cliError("tag_skill_toggle_failed");
        }

        std::ostringstream out;
        out << "skill=" << cliEscapeValue(skill_name(skill)) << "\n";
        out << "tagged=" << (wasTagged ? 0 : 1) << "\n";
        out << "remaining_tag_skills=" << editor_get_remaining_tag_skill_count();
        return cliOk(out.str());
    }

    if (command == "trait_select") {
        if (tokens.size() < 2) {
            return cliError("usage=trait_select <trait_name_or_index>");
        }

        if (!editor_is_creation_mode()) {
            return cliError("trait_select_available_only_in_chargen");
        }

        int trait = cliParseTrait(cliJoinTokens(tokens, 1));
        if (trait < 0) {
            return cliError("invalid_trait");
        }

        bool wasSelected = cliHasSelectedTrait(trait);
        if (!wasSelected && editor_get_remaining_trait_count() <= 0) {
            return cliError("no_trait_slots_remaining");
        }

        if (editor_cli_toggle_trait(trait) != 0) {
            return cliError("trait_toggle_failed");
        }

        std::ostringstream out;
        out << "trait=" << cliEscapeValue(trait_name(trait)) << "\n";
        out << "selected=" << (wasSelected ? 0 : 1) << "\n";
        out << "remaining_traits=" << editor_get_remaining_trait_count();
        return cliOk(out.str());
    }

    if (command == "set_name") {
        if (tokens.size() < 2) {
            return cliError("usage=set_name <name>");
        }

        std::string name = cliJoinTokens(tokens, 1);
        if (name.empty()) {
            return cliError("empty_name");
        }

        GNW_add_input_buffer(517);

        int sentChars = 0;
        for (char ch : name) {
            if (sentChars >= 11) {
                break;
            }

            unsigned char uch = static_cast<unsigned char>(ch);
            if (uch >= KEY_FIRST_INPUT_CHARACTER && uch <= KEY_LAST_INPUT_CHARACTER && isdoschar(uch)) {
                GNW_add_input_buffer(uch);
                sentChars++;
            }
        }

        GNW_add_input_buffer(KEY_RETURN);

        std::ostringstream out;
        out << "name_input_sent=1 chars=" << sentChars;
        return cliOk(out.str());
    }

    if (command == "done") {
        if (editor_is_active() && editor_is_creation_mode()) {
            int remainingPoints = character_points;
            int remainingTagSkills = editor_get_remaining_tag_skill_count();
            bool hasInvalidSpecialStats = editor_has_invalid_special_stats();

            if (remainingPoints > 0 || remainingTagSkills > 0 || hasInvalidSpecialStats) {
                std::ostringstream out;
                out << "done_ready=0\n";
                out << "remaining_character_points=" << remainingPoints << "\n";
                out << "remaining_tag_skills=" << remainingTagSkills << "\n";
                out << "special_over_10=" << (hasInvalidSpecialStats ? 1 : 0);
                return cliError(out.str());
            }
        }

        return cliQueueKey(KEY_RETURN);
    }

    if (command == "move") {
        if (tokens.size() < 2) {
            return cliError("usage=move <direction>");
        }

        if (obj_dude == NULL) {
            return cliError("player_unavailable");
        }

        int rotation = cliDirectionFromString(tokens[1]);
        if (rotation < 0) {
            return cliError("invalid_direction");
        }

        int destination = tile_num_in_direction(obj_dude->tile, rotation, 1);
        int actionPoints = isInCombat() ? obj_dude->data.critter.combat.ap : -1;
        if (register_begin(ANIMATION_REQUEST_UNRESERVED) != 0) {
            return cliError("move_failed");
        }

        if (register_object_move_to_tile(obj_dude, destination, obj_dude->elevation, actionPoints, 0) != 0) {
            register_end();
            return cliError("move_failed");
        }

        if (register_end() != 0) {
            return cliError("move_failed");
        }

        tile_scroll_to(destination, 2);

        std::ostringstream out;
        out << "destination_tile=" << destination;
        return cliOk(out.str());
    }

    if (command == "move_to") {
        if (tokens.size() < 2) {
            return cliError("usage=move_to <tile>");
        }

        if (obj_dude == NULL) {
            return cliError("player_unavailable");
        }

        int destination;
        if (!cliParseInteger(tokens[1], &destination)) {
            return cliError("invalid_tile");
        }

        if (destination < 0 || destination >= HEX_GRID_SIZE) {
            return cliError("tile_out_of_range");
        }

        int actionPoints = isInCombat() ? obj_dude->data.critter.combat.ap : -1;
        if (register_begin(ANIMATION_REQUEST_UNRESERVED) != 0) {
            return cliError("move_failed");
        }

        if (register_object_move_to_tile(obj_dude, destination, obj_dude->elevation, actionPoints, 0) != 0) {
            register_end();
            return cliError("move_failed");
        }

        if (register_end() != 0) {
            return cliError("move_failed");
        }

        tile_scroll_to(destination, 2);

        std::ostringstream out;
        out << "destination_tile=" << destination;
        return cliOk(out.str());
    }

    if (command == "goto") {
        if (tokens.size() < 2) {
            return cliError("usage=goto <object_id_or_tile>");
        }

        if (obj_dude == NULL) {
            return cliError("player_unavailable");
        }

        int targetIdOrTile;
        if (!cliParseInteger(tokens[1], &targetIdOrTile)) {
            return cliError("invalid_target");
        }

        if (!cliWaitForObjectAnimationToComplete(obj_dude, kCliGotoWaitTimeoutMs)) {
            return cliError("animation_timeout");
        }

        int playerTile = obj_dude->tile;
        int playerElevation = obj_dude->elevation;

        Object* targetObject = cliFindWorldObjectById(targetIdOrTile);
        bool targetIsObject = targetObject != NULL;

        int targetTile = playerTile;
        int destinationTile = playerTile;
        int plannedSteps = 0;
        bool capped = false;
        bool arrivedAdjacent = false;
        bool shouldMove = false;
        bool partialResult = false;

        if (targetIsObject) {
            targetTile = targetObject->tile;
            if (targetObject->elevation != playerElevation) {
                std::ostringstream out;
                out << "different_elevation\n";
                out << "player_elevation=" << playerElevation << "\n";
                out << "target_elevation=" << targetObject->elevation;
                return cliError(out.str());
            }

            if (obj_dist(obj_dude, targetObject) <= 1) {
                arrivedAdjacent = true;
            } else {
                unsigned char rotations[kCliGotoPathRotationsCapacity];
                int closestTile = playerTile;
                int pathLength = cliMakePathWithClosest(obj_dude, playerTile, targetTile, rotations, 0, &closestTile);
                closestTile = cliValidateClosestTile(obj_dude, playerTile, closestTile);
                if (pathLength == 0) {
                    if (cliPlanFallbackMove(obj_dude, playerTile, closestTile, &destinationTile, &plannedSteps, &capped)) {
                        shouldMove = true;
                        partialResult = true;
                    } else {
                        std::ostringstream out;
                        out << "unreachable\n";
                        out << "closest_tile=" << closestTile << "\n";
                        out << "distance_from_target=" << tile_dist(closestTile, targetTile);
                        return cliError(out.str());
                    }
                } else {
                    int stopDistance = (targetObject->flags & OBJECT_MULTIHEX) != 0 ? 2 : 1;
                    int stepsToAdjacent = pathLength - stopDistance;
                    if (stepsToAdjacent <= 0) {
                        arrivedAdjacent = true;
                    } else {
                        plannedSteps = std::min(stepsToAdjacent, kCliGotoMaxPathLength);
                        capped = plannedSteps < stepsToAdjacent;
                        destinationTile = cliAdvanceAlongPath(playerTile, rotations, plannedSteps);
                        shouldMove = plannedSteps > 0;
                    }
                }
            }
        } else {
            if (targetIdOrTile < 0 || targetIdOrTile >= HEX_GRID_SIZE) {
                return cliError("tile_out_of_range");
            }

            targetTile = targetIdOrTile;

            if (playerTile != targetTile) {
                bool targetBlocked = obj_blocking_at(obj_dude, targetTile, playerElevation) != NULL;
                unsigned char rotations[kCliGotoPathRotationsCapacity];
                int closestTile = playerTile;
                int pathLength = cliMakePathWithClosest(obj_dude, playerTile, targetTile, rotations, targetBlocked ? 0 : 1, &closestTile);
                closestTile = cliValidateClosestTile(obj_dude, playerTile, closestTile);

                if (!targetBlocked) {
                    if (pathLength == 0) {
                        if (cliPlanFallbackMove(obj_dude, playerTile, closestTile, &destinationTile, &plannedSteps, &capped)) {
                            shouldMove = true;
                            partialResult = true;
                        } else {
                            std::ostringstream out;
                            out << "unreachable\n";
                            out << "closest_tile=" << closestTile << "\n";
                            out << "distance_from_target=" << tile_dist(closestTile, targetTile);
                            return cliError(out.str());
                        }
                    } else {
                        plannedSteps = std::min(pathLength, kCliGotoMaxPathLength);
                        capped = plannedSteps < pathLength;
                        destinationTile = capped ? cliAdvanceAlongPath(playerTile, rotations, plannedSteps) : targetTile;
                        shouldMove = plannedSteps > 0;
                    }
                } else {
                    if (pathLength > 0) {
                        int stepsToNearestWalkable = pathLength - 1;
                        plannedSteps = std::min(stepsToNearestWalkable, kCliGotoMaxPathLength);
                        capped = plannedSteps < stepsToNearestWalkable;
                        destinationTile = plannedSteps > 0 ? cliAdvanceAlongPath(playerTile, rotations, plannedSteps) : playerTile;
                        shouldMove = plannedSteps > 0;
                    } else {
                        if (cliPlanFallbackMove(obj_dude, playerTile, closestTile, &destinationTile, &plannedSteps, &capped)) {
                            shouldMove = true;
                            partialResult = true;
                        }

                        if (!shouldMove && tile_dist(playerTile, targetTile) > 0) {
                            std::ostringstream out;
                            out << "unreachable\n";
                            out << "closest_tile=" << closestTile << "\n";
                            out << "distance_from_target=" << tile_dist(closestTile, targetTile);
                            return cliError(out.str());
                        }
                    }
                }
            }
        }

        if (shouldMove) {
            int actionPoints = isInCombat() ? obj_dude->data.critter.combat.ap : -1;

            if (register_begin(ANIMATION_REQUEST_UNRESERVED) != 0) {
                return cliError("move_failed");
            }

            if (register_object_move_to_tile(obj_dude, destinationTile, playerElevation, actionPoints, 0) != 0) {
                register_end();
                return cliError("move_failed");
            }

            if (register_end() != 0) {
                return cliError("move_failed");
            }

            if (!cliWaitForObjectAnimationToComplete(obj_dude, kCliGotoWaitTimeoutMs)) {
                return cliError("animation_timeout");
            }

            tile_scroll_to(obj_dude->tile, 2);
        }

        int finalTile = obj_dude->tile;
        int distanceFromTarget = tile_dist(finalTile, targetTile);
        if (targetIsObject) {
            arrivedAdjacent = obj_dist(obj_dude, targetObject) <= 1;
        }

        std::ostringstream out;
        if (partialResult) {
            out << "result=partial\n";
        }
        out << "target_kind=" << (targetIsObject ? "object" : "tile") << "\n";
        if (targetIsObject) {
            out << "target_object_id=" << targetObject->id << "\n";
        }
        out << "target_tile=" << targetTile << "\n";
        out << "destination_tile=" << destinationTile << "\n";
        out << "planned_steps=" << plannedSteps << "\n";
        out << "capped=" << (capped ? 1 : 0) << "\n";
        out << "final_tile=" << finalTile << "\n";
        out << "distance_from_target=" << distanceFromTarget << "\n";
        out << "arrived_adjacent=" << (arrivedAdjacent ? 1 : 0);
        return cliOk(out.str());
    }

    if (command == "enter") {
        if (obj_dude == NULL) {
            return cliError("player_unavailable");
        }

        Object* exitGrid = cliFindNearestExitGrid(999);
        if (exitGrid == NULL) {
            return cliError("exit_grid_not_found");
        }

        int exitGridId = exitGrid->id;
        int exitGridTile = exitGrid->tile;
        int exitGridDistance = tile_dist(obj_dude->tile, exitGridTile);

        std::ostringstream out;
        out << "exit_grid_pid=0x" << std::hex << exitGrid->pid << std::dec << "\n";
        out << "exit_grid_tile=" << exitGridTile << "\n";
        out << "exit_grid_distance=" << exitGridDistance << "\n";
        out << "exit_grid_object_id=" << exitGridId << "\n";

        if (obj_move_to_tile(obj_dude, exitGridTile, obj_dude->elevation, NULL) != 0) {
            out << "entered_exit_grid=0";
            return cliError(out.str());
        }

        out << "entered_exit_grid=1\n";
        out << "object_id=" << exitGridId << "\n";
        out << "tile=" << exitGridTile;
        return cliOk(out.str());
    }

    if (command == "scan_exits") {
        if (obj_dude == NULL) {
            return cliError("player_unavailable");
        }

        std::vector<NearbyObjectInfo> exitGrids;

        Object** objects = NULL;
        int objectCount = obj_create_list(-1, obj_dude->elevation, OBJ_TYPE_MISC, &objects);
        if (objectCount < 0) {
            return cliError("scan_failed");
        }

        for (int index = 0; index < objectCount; index++) {
            Object* object = objects[index];
            if (!cliIsExitGrid(object)) {
                continue;
            }

            NearbyObjectInfo info;
            info.object = object;
            info.distance = tile_dist(obj_dude->tile, object->tile);
            info.direction = -1;
            exitGrids.push_back(info);
        }

        if (objectCount > 0) {
            obj_delete_list(objects);
        }

        if (exitGrids.empty()) {
            std::vector<Object*> allObjects = cliCollectObjectsAtElevation(obj_dude->elevation);
            for (Object* object : allObjects) {
                if (!cliIsExitGrid(object)) {
                    continue;
                }

                NearbyObjectInfo info;
                info.object = object;
                info.distance = tile_dist(obj_dude->tile, object->tile);
                info.direction = -1;
                exitGrids.push_back(info);
            }
        }

        std::sort(exitGrids.begin(), exitGrids.end(), [](const NearbyObjectInfo& lhs, const NearbyObjectInfo& rhs) {
            if (lhs.distance != rhs.distance) {
                return lhs.distance < rhs.distance;
            }

            return lhs.object->id < rhs.object->id;
        });

        std::ostringstream out;
        out << "count=" << exitGrids.size();
        for (const NearbyObjectInfo& info : exitGrids) {
            Object* object = info.object;
            out << "\n[" << object->id << "] ";
            out << "pid=0x" << std::hex << object->pid << std::dec;
            out << " tile=" << object->tile;
            out << " distance=" << info.distance;
        }

        return cliOk(out.str());
    }

    if (command == "interact" || command == "talk" || command == "pickup") {
        if (tokens.size() < 2) {
            return cliError("usage=interact|talk|pickup <object_id>");
        }

        int objectId;
        if (!cliParseInteger(tokens[1], &objectId)) {
            return cliError("invalid_object_id");
        }

        Object* target = cliFindWorldObjectById(objectId);
        if (target == NULL) {
            return cliError("object_not_found");
        }

        int rc;
        if (command == "interact") {
            rc = action_use_an_object(obj_dude, target);
        } else if (command == "talk") {
            rc = action_talk_to(obj_dude, target);
        } else {
            rc = action_get_an_object(obj_dude, target);
        }

        if (rc != 0) {
            return cliError("action_failed");
        }

        return cliOk("action_started=1");
    }

    if (command == "use_skill") {
        if (tokens.size() < 3) {
            return cliError("usage=use_skill <skill_name> <target_id>");
        }

        int targetId;
        if (!cliParseInteger(tokens.back(), &targetId)) {
            return cliError("invalid_target_id");
        }

        std::string skillName = cliJoinTokens(tokens, 1);
        size_t splitPos = skillName.rfind(' ');
        if (splitPos == std::string::npos) {
            return cliError("invalid_skill_name");
        }

        skillName = skillName.substr(0, splitPos);
        int skill = cliParseSkill(skillName);
        if (skill < 0) {
            return cliError("invalid_skill_name");
        }

        Object* target = cliFindWorldObjectById(targetId);
        if (target == NULL) {
            return cliError("target_not_found");
        }

        if (action_use_skill_on(obj_dude, target, skill) != 0) {
            return cliError("skill_use_failed");
        }

        return cliOk("action_started=1");
    }

    if (command == "wait") {
        if (tokens.size() < 2) {
            return cliError("usage=wait <hours>");
        }

        int hours;
        if (!cliParseInteger(tokens[1], &hours) || hours <= 0) {
            return cliError("invalid_hours");
        }

        if (isInCombat()) {
            return cliError("cannot_wait_in_combat");
        }

        long seconds = static_cast<long>(hours) * 3600L;
        if (seconds > INT_MAX) {
            seconds = INT_MAX;
        }

        inc_game_time_in_seconds(static_cast<int>(seconds));
        partyMemberRestingHeal(hours);

        std::ostringstream out;
        out << "hours_advanced=" << hours;
        return cliOk(out.str());
    }

    if (command == "attack") {
        if (tokens.size() < 2) {
            return cliError("usage=attack <target_id> [body_part]");
        }

        int targetId;
        if (!cliParseInteger(tokens[1], &targetId)) {
            return cliError("invalid_target_id");
        }

        Object* target = cliFindWorldObjectById(targetId);
        if (target == NULL) {
            return cliError("target_not_found");
        }

        if (tokens.size() >= 3) {
            int hitLocation = cliParseHitLocation(tokens[2]);
            if (hitLocation < 0) {
                return cliError("invalid_body_part");
            }

            if (!isInCombat()) {
                combat_attack_this(target);
                return cliOk("combat_started=1 body_part_ignored_until_combat");
            }

            if (combat_whose_turn() != obj_dude) {
                return cliError("not_players_turn");
            }

            int hitMode;
            bool aiming;
            if (intface_get_attack(&hitMode, &aiming) == -1) {
                return cliError("cannot_get_attack_mode");
            }

            if (combat_attack(obj_dude, target, hitMode, hitLocation) == -1) {
                return cliError("attack_failed");
            }

            return cliOk("attack_started=1");
        }

        combat_attack_this(target);
        return cliOk("attack_started=1");
    }

    if (command == "end_turn") {
        if (!isInCombat()) {
            return cliError("not_in_combat");
        }

        combat_end_turn();
        return cliOk("turn_ended=1");
    }

    if (command == "reload") {
        Object* weapon = NULL;
        if (intface_get_current_item(&weapon) == -1 || weapon == NULL) {
            return cliError("no_active_item");
        }

        if (item_get_type(weapon) != ITEM_TYPE_WEAPON) {
            return cliError("active_item_not_weapon");
        }

        if (item_w_try_reload(obj_dude, weapon) == -1) {
            return cliError("reload_failed");
        }

        if (isInCombat()) {
            int hitMode = intface_is_item_right_hand() ? HIT_MODE_RIGHT_WEAPON_RELOAD : HIT_MODE_LEFT_WEAPON_RELOAD;
            int actionPoints = item_mp_cost(obj_dude, hitMode, false);
            if (actionPoints > obj_dude->data.critter.combat.ap) {
                obj_dude->data.critter.combat.ap = 0;
            } else {
                obj_dude->data.critter.combat.ap -= actionPoints;
            }
            intface_update_move_points(obj_dude->data.critter.combat.ap, combat_free_move);
        }

        intface_update_items(false);
        return cliOk("reloaded=1");
    }

    if (command == "change_weapon") {
        if (intface_toggle_items(true) == -1) {
            return cliError("change_weapon_failed");
        }

        return cliOk("weapon_changed=1");
    }

    if (command == "flee") {
        if (!isInCombat()) {
            return cliError("not_in_combat");
        }

        combat_end();
        return cliOk("flee_attempted=1");
    }

    if (command == "say") {
        if (!dialog_active()) {
            return cliError("not_in_dialogue");
        }

        if (tokens.size() < 2) {
            return cliError("usage=say <option_number>");
        }

        int option;
        if (!cliParseInteger(tokens[1], &option) || option <= 0) {
            return cliError("invalid_option_number");
        }

        if (gdialog_select_option(option - 1) == -1) {
            return cliError("option_selection_failed");
        }

        return cliOk("option_selected=1");
    }

    if (command == "barter") {
        if (!dialog_active()) {
            return cliError("not_in_dialogue");
        }

        return cliQueueKey(KEY_LOWERCASE_B);
    }

    if (command == "end") {
        if (!dialog_active()) {
            return cliError("not_in_dialogue");
        }

        int optionCount = gdialog_get_option_count();
        if (optionCount <= 0) {
            return cliError("no_dialogue_options");
        }

        int chosen = optionCount - 1;
        for (int index = 0; index < optionCount; index++) {
            std::string option = cliToLower(cliStripDialogPrefix(gdialog_get_option_text(index)));
            if (option.find("goodbye") != std::string::npos
                || option.find("bye") != std::string::npos
                || option.find("leave") != std::string::npos
                || option.find("done") != std::string::npos) {
                chosen = index;
                break;
            }
        }

        if (gdialog_select_option(chosen) == -1) {
            return cliError("option_selection_failed");
        }

        return cliOk("option_selected=1");
    }

    if (command == "inventory") {
        return cliOk(cliBuildInventoryDump());
    }

    if (command == "equip") {
        if (tokens.size() < 3) {
            return cliError("usage=equip <item_id> <slot>");
        }

        int itemId;
        if (!cliParseInteger(tokens[1], &itemId)) {
            return cliError("invalid_item_id");
        }

        std::string slot = cliToLower(tokens[2]);
        Object* item = cliFindPlayerItemById(itemId);
        if (item == NULL) {
            return cliError("item_not_found");
        }

        int rc = -1;
        if (slot == "left_hand") {
            rc = inven_wield(obj_dude, item, 0);
        } else if (slot == "right_hand") {
            rc = inven_wield(obj_dude, item, 1);
        } else if (slot == "armor") {
            if (item_get_type(item) != ITEM_TYPE_ARMOR) {
                return cliError("item_is_not_armor");
            }

            rc = inven_wield(obj_dude, item, 0);
        } else {
            return cliError("invalid_slot");
        }

        if (rc != 0) {
            return cliError("equip_failed");
        }

        intface_update_items(false);
        intface_update_ac(true);

        return cliOk("equipped=1");
    }

    if (command == "unequip") {
        if (tokens.size() < 2) {
            return cliError("usage=unequip <slot>");
        }

        std::string slot = cliToLower(tokens[1]);

        if (slot == "left_hand") {
            if (inven_unwield(obj_dude, 0) != 0) {
                return cliError("unequip_failed");
            }
        } else if (slot == "right_hand") {
            if (inven_unwield(obj_dude, 1) != 0) {
                return cliError("unequip_failed");
            }
        } else if (slot == "armor") {
            Object* armor = inven_worn(obj_dude);
            if (armor == NULL) {
                return cliError("no_armor_equipped");
            }

            armor->flags &= ~OBJECT_WORN;
            adjust_ac(obj_dude, armor, NULL);

            Proto* proto;
            if (proto_ptr(obj_dude->pid, &proto) != -1) {
                int baseFrmId = proto->fid & 0xFFF;
                int fid = art_id(OBJ_TYPE_CRITTER,
                    baseFrmId,
                    0,
                    (obj_dude->fid & 0xF000) >> 12,
                    obj_dude->rotation + 1);
                obj_change_fid(obj_dude, fid, NULL);
            }
        } else {
            return cliError("invalid_slot");
        }

        intface_update_items(false);
        intface_update_ac(true);

        return cliOk("unequipped=1");
    }

    if (command == "use") {
        if (tokens.size() < 2) {
            return cliError("usage=use <item_id>");
        }

        int itemId;
        if (!cliParseInteger(tokens[1], &itemId)) {
            return cliError("invalid_item_id");
        }

        Object* item = cliFindPlayerItemById(itemId);
        if (item == NULL) {
            return cliError("item_not_found");
        }

        if (obj_use_item(obj_dude, item) == -1) {
            return cliError("use_failed");
        }

        intface_update_items(false);
        return cliOk("used=1");
    }

    if (command == "drop") {
        if (tokens.size() < 2) {
            return cliError("usage=drop <item_id>");
        }

        int itemId;
        if (!cliParseInteger(tokens[1], &itemId)) {
            return cliError("invalid_item_id");
        }

        Object* item = cliFindPlayerItemById(itemId);
        if (item == NULL) {
            return cliError("item_not_found");
        }

        if (obj_drop(obj_dude, item) == -1) {
            return cliError("drop_failed");
        }

        intface_update_items(false);
        return cliOk("dropped=1");
    }

    if (command == "examine") {
        if (tokens.size() < 2) {
            return cliError("usage=examine <item_id>");
        }

        int objectId;
        if (!cliParseInteger(tokens[1], &objectId)) {
            return cliError("invalid_item_id");
        }

        Object* object = cliFindAnyObjectById(objectId);
        if (object == NULL) {
            return cliError("object_not_found");
        }

        const char* name = object_name(object);
        const char* description = object_description(object);
        if (description == NULL || description[0] == '\0') {
            description = item_description(object);
        }

        std::ostringstream out;
        out << "name=" << cliEscapeValue(name != NULL ? name : "") << "\n";
        out << "description=" << cliEscapeValue(description != NULL ? description : "") << "\n";
        return cliOk(out.str());
    }

    if (command == "worldmap") {
        MapTransition transition;
        transition.map = 0;
        transition.elevation = -1;
        transition.tile = -1;
        transition.rotation = 0;

        if (map_leave_map(&transition) != 0) {
            return cliError("worldmap_transition_failed");
        }

        return cliOk("worldmap_requested=1");
    }

    if (command == "travel") {
        if (tokens.size() < 2) {
            return cliError("usage=travel <location_name>");
        }

        if (!worldmap_is_active()) {
            return cliError("not_on_worldmap");
        }

        std::string townName = cliJoinTokens(tokens, 1);
        int town = worldmap_find_town_by_name(townName.c_str());
        if (town < 0) {
            return cliError("unknown_location");
        }

        if (!worldmap_is_town_known(town)) {
            return cliError("location_not_known");
        }

        GNW_add_input_buffer(500 + town);

        std::ostringstream out;
        out << "travel_requested=" << town;
        return cliOk(out.str());
    }

    if (command == "cancel") {
        if (!worldmap_is_active()) {
            return cliError("not_on_worldmap");
        }

        return cliQueueKey(KEY_ESCAPE);
    }

    if (command == "save") {
        if (tokens.size() < 2) {
            return cliError("usage=save <slot>");
        }

        int slot;
        if (!cliParseInteger(tokens[1], &slot)) {
            return cliError("invalid_slot");
        }

        if (slot >= 1 && slot <= 10) {
            slot -= 1;
        }

        if (slot < 0 || slot > 9) {
            return cliError("slot_out_of_range");
        }

        loadsave_set_quick_slot(slot);

        int rc = SaveGame(LOAD_SAVE_MODE_QUICK);
        if (rc != 1) {
            return cliError("save_failed");
        }

        std::ostringstream out;
        out << "saved_slot=" << (slot + 1);
        return cliOk(out.str());
    }

    if (command == "pipboy") {
        return cliQueueKey(KEY_LOWERCASE_P);
    }

    if (command == "character") {
        return cliQueueKey(KEY_LOWERCASE_C);
    }

    if (command == "automap") {
        return cliQueueKey(KEY_TAB);
    }

    if (command == "sneak") {
        if (action_skill_use(SKILL_SNEAK) != 0) {
            return cliError("sneak_toggle_failed");
        }

        return cliOk("sneak_toggled=1");
    }

    return cliError("unknown_command");
}

void cliWriteOutput(const std::string& output)
{
    std::ofstream stream(kCliOutputPath, std::ios::out | std::ios::trunc);
    if (!stream.good()) {
        return;
    }

    stream << output;
    stream.flush();
}

void cliWriteResponse(const std::string& command, const CliCommandResponse& response)
{
    std::ostringstream out;

    out << "[RESULT]\n";
    out << "status=" << (response.ok ? "ok" : "error") << "\n";
    out << "command=" << command << "\n";
    out << "\n";
    out << response.body << "\n";

    cliWriteOutput(out.str());
}

int cliEnsurePipeExists()
{
    struct stat st;
    if (stat(kCliInputPipePath, &st) == 0) {
        if (!S_ISFIFO(st.st_mode)) {
            if (unlink(kCliInputPipePath) != 0) {
                return -1;
            }

            if (mkfifo(kCliInputPipePath, 0666) != 0) {
                return -1;
            }
        }
    } else {
        if (errno != ENOENT) {
            return -1;
        }

        if (mkfifo(kCliInputPipePath, 0666) != 0 && errno != EEXIST) {
            return -1;
        }
    }

    return 0;
}

void cliCloseInputPipe()
{
    if (gCliInputFd != -1) {
        close(gCliInputFd);
        gCliInputFd = -1;
    }
}

int cliOpenInputPipe()
{
    if (gCliInputFd != -1) {
        return 0;
    }

    if (cliEnsurePipeExists() != 0) {
        return -1;
    }

    gCliInputFd = open(kCliInputPipePath, O_RDONLY | O_NONBLOCK);
    if (gCliInputFd == -1) {
        return -1;
    }

    return 0;
}

void cliPollInput()
{
    if (cliOpenInputPipe() != 0) {
        return;
    }

    char buffer[1024];
    while (true) {
        ssize_t bytesRead = read(gCliInputFd, buffer, sizeof(buffer));
        if (bytesRead > 0) {
            gCliInputBuffer.append(buffer, bytesRead);
            continue;
        }

        if (bytesRead == 0) {
            cliCloseInputPipe();
            break;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        cliCloseInputPipe();
        break;
    }

    size_t newlinePos;
    while ((newlinePos = gCliInputBuffer.find('\n')) != std::string::npos) {
        std::string command = gCliInputBuffer.substr(0, newlinePos);
        gCliInputBuffer.erase(0, newlinePos + 1);

        if (!command.empty() && command.back() == '\r') {
            command.pop_back();
        }

        command = cliTrim(command);
        if (command.empty()) {
            continue;
        }

        CliCommandResponse response = cliExecuteCommand(command);
        cliWriteResponse(command, response);
    }
}

} // namespace

bool cli_is_enabled()
{
    return gCliEnabled;
}

void cli_set_enabled(bool enabled)
{
    gCliEnabled = enabled;
}

int cli_init()
{
    if (!gCliEnabled) {
        return 0;
    }

    gCliInputBuffer.clear();
    cliCloseInputPipe();

    if (cliOpenInputPipe() != 0) {
        cliWriteOutput("[RESULT]\nstatus=error\ncommand=init\n\nfailed_to_open_cli_pipe\n");
        return -1;
    }

    cliWriteOutput("[RESULT]\nstatus=ok\ncommand=init\n\ncli_ready=1\n");

    return 0;
}

void cli_exit()
{
    cliCloseInputPipe();
    gCliInputBuffer.clear();
}

void cli_process_bk()
{
    if (!gCliEnabled) {
        return;
    }

    cliPollInput();
}

} // namespace fallout
