/*
 * mod-retail-customization
 *
 * Stores the Retail character customization choices (Eyebrows, Ears, Eye Color, ...) that a FlashWOW
 * client sends with a new character, and hands them to every client that shows that character, so the
 * look a player picked is the look everyone (players and bots alike) sees.
 *
 * The 3.3.5 skin, face, hair style, hair colour and facial hair values still travel in the stock
 * fields; this module only carries the options those five cannot express.
 *
 * Protocol (all little endian):
 *
 *   CMSG_CHAR_CREATE (0x036), after the stock fields:
 *   CMSG_ALTER_APPEARANCE (0x426), after the stock fields:
 *       u32 magic 'RTLC' (0x434C5452), u8 count, count x (u32 optionId, u32 choiceId)
 *     Stock clients send nothing after the stock fields and are unaffected.
 *
 *   CMSG_UNUSED5 (0x4B8) "query", in world only:
 *       u8 count, count x u64 guid
 *
 *   0x4B9 "choices" (server to client):
 *       u16 entries, entries x (u64 guid, u8 count, count x (u32 optionId, u32 choiceId))
 *     Sent in reply to a query (a guid without stored choices comes back with count 0), for all of
 *     an account's characters when its client asks for the character list, and to everyone nearby
 *     when a barber shop visit or a form change changes a player's choices.
 *     In the character list, option 0xFFFFFF01 (not a Retail option) carries the Two Forms form.
 *
 * Two Forms (FlashWOW): a race with an altered form (Worgen -> Gilnean, Dracthyr -> Visage; AlteredFormRace in
 * SharedDefines.h) keeps a second set of the five appearance bytes and Retail choices here. The
 * active form's set is in the normal places (characters, character_retail_customization); the other
 * is parked in character_two_forms(_choices). Changing form swaps the two and sets or clears
 * UNIT_FLAG2_ALTERED_FORM, which tells every client to draw the altered form's race. The form is
 * changed by casting TwoForms.ToggleSpell (not in combat); TwoForms.NativeFormSpells return a player
 * to the race's own form (Darkflight, Running Wild).
 *
 * Playerbots: a random bot (an account named with RetailCustomization.BotAccountPrefix) that has no stored
 * choices is given a look at its first login, one random choice per option from retail_customization_bot_pool
 * (world database, written by the FlashWOW client's retailBotLooks command), and keeps it from then on.
 */

#include "AccountMgr.h"
#include "Chat.h"
#include "Config.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr uint32 kMagic = 0x434C5452;             // "RTLC"
    constexpr uint16 kQueryOpcode = 0x4B8;            // CMSG_UNUSED5
    constexpr uint16 kChoicesOpcode = 0x4B9;          // free on the client; sent by the server only
    constexpr uint8 kMaxChoices = 64;
    constexpr uint8 kMaxQuery = 64;
    constexpr uint32 kSavedFormOption = 0xFFFFFF01;  // character list only: the character's Two Forms form
    // Character list only: the other Two Forms form's look, for the character select screen. Skin, face, hair
    // style and hair colour packed low byte first; facial hair; its Retail choices with the top bit set on the option.
    constexpr uint32 kOtherLookOption = 0xFFFFFF02;
    constexpr uint32 kOtherFacialOption = 0xFFFFFF03;
    constexpr uint32 kOtherChoiceBit = 0x80000000;
    constexpr uint32 kFormMagic = 0x464F5754;         // "TWOF": a new character's altered form look

    using Choices = std::map<uint32, uint32>;     // optionId -> choiceId

    struct Stored
    {
        uint32 account = 0;
        Choices choices;
    };

    bool s_enabled = true;
    std::mutex s_mutex;
    std::unordered_map<ObjectGuid::LowType, Stored> s_stored;
    std::unordered_map<uint32, Choices> s_pendingCreate;      // by account
    std::unordered_map<ObjectGuid::LowType, Choices> s_pendingBarber;

    // Two Forms: the form a character shows and its other form's look.
    struct Forms
    {
        uint32 account = 0;
        uint8 form = 0;             // 0 the race's own form is active, 1 its altered form
        uint8 parked[5] = {};       // the other form's skin, face, hair style, hair colour, facial hair
        Choices parkedChoices;      // the other form's Retail choices
    };
    std::unordered_map<ObjectGuid::LowType, Forms> s_forms;

    // `form` < 0: no form entry (only the character list carries one). `other`: the other form's look, for the
    // character list.
    void PutEntry(WorldPacket& data, uint64 guid, Choices const* choices, int form = -1, Forms const* other = nullptr)
    {
        size_t count = (choices ? std::min<size_t>(choices->size(), kMaxChoices) : 0);
        size_t otherCount = (other ? std::min<size_t>(other->parkedChoices.size(), kMaxChoices) : 0);
        data << guid;
        data << uint8(count + (form >= 0 ? 1 : 0) + (other ? 2 + otherCount : 0));
        if (choices)
            for (auto const& [option, choice] : *choices)
            {
                if (!count--)
                    break;
                data << option << choice;
            }
        if (form >= 0)
            data << kSavedFormOption << uint32(form);
        if (other)
        {
            data << kOtherLookOption << uint32(other->parked[0] | (other->parked[1] << 8) | (other->parked[2] << 16) | (uint32(other->parked[3]) << 24));
            data << kOtherFacialOption << uint32(other->parked[4]);
            for (auto const& [option, choice] : other->parkedChoices)
            {
                if (!otherCount--)
                    break;
                data << (option | kOtherChoiceBit) << choice;
            }
        }
    }
    std::unordered_map<uint32, Forms> s_pendingForms;     // by account, from the create screen
    std::set<uint32> s_toggleSpells;
    bool s_nativeFormInCombat = true;
    std::set<uint32> s_nativeFormInCombatRaces;
    std::map<uint32, uint32> s_raceFormSpells;                // race -> its toggle spell, taught at login
    std::set<uint32> s_nativeFormSpells;

    // Playerbots: the choices a race, sex and class may take, by option (retail_customization_bot_pool).
    bool s_botLooks = true;
    std::string s_botAccountPrefix = "RNDBOT";
    using Pool = std::vector<std::pair<uint32, std::vector<uint32>>>;
    std::unordered_map<uint32, Pool> s_botPool;
    std::unordered_map<uint32, bool> s_botAccounts;          // account -> is a random bot account

    uint32 PoolKey(uint32 race, uint32 sex, uint32 cls)
    {
        return (race << 16) | (sex << 8) | cls;
    }

    // A random bot: logged in without a client, on an account named with the bot prefix (asked once per account).
    bool IsRandomBot(Player* player)
    {
        WorldSession* session = player->GetSession();
        if (!session || !session->IsSocketClosed() || s_botAccountPrefix.empty())
            return false;
        uint32 const account = session->GetAccountId();
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            auto const known = s_botAccounts.find(account);
            if (known != s_botAccounts.end())
                return known->second;
        }
        std::string name;
        bool const bot = AccountMgr::GetName(account, name) && name.size() >= s_botAccountPrefix.size() &&
            std::equal(s_botAccountPrefix.begin(), s_botAccountPrefix.end(), name.begin(),
                [](char a, char b) { return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b)); });
        std::lock_guard<std::mutex> lock(s_mutex);
        s_botAccounts[account] = bot;
        return bot;
    }

    // Reads the trailing RTLC block at the packet's read position. False when there is none or it
    // is malformed (the stock data is left alone either way).
    bool ReadBlock(WorldPacket& packet, Choices& choices)
    {
        choices.clear();
        if (packet.size() - packet.rpos() < 5)
            return false;
        uint32 magic = 0;
        uint8 count = 0;
        packet >> magic;
        if (magic != kMagic)
            return false;
        packet >> count;
        if (count > kMaxChoices || packet.size() - packet.rpos() < size_t(count) * 8)
            return false;
        for (uint8 i = 0; i < count; ++i)
        {
            uint32 option = 0, choice = 0;
            packet >> option >> choice;
            if (option && choice)
                choices[option] = choice;
        }
        return true;
    }

    void Save(ObjectGuid::LowType low, uint32 account, Choices const& choices)
    {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            Stored& stored = s_stored[low];
            stored.account = account;
            stored.choices = choices;
        }
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append("DELETE FROM `character_retail_customization` WHERE `guid` = {}", low);
        for (auto const& [option, choice] : choices)
            trans->Append("INSERT INTO `character_retail_customization` (`guid`, `option_id`, `choice_id`) VALUES ({}, {}, {})",
                low, option, choice);
        CharacterDatabase.CommitTransaction(trans);
    }

    // `form` >= 0 tags the choices with the Two Forms form they belong to (a form change: clients hold them until
    // the form field changes, so the old body keeps its look).
    WorldPacket ChoicesPacket(ObjectGuid::LowType low, Choices const& choices, int form = -1)
    {
        WorldPacket data(kChoicesOpcode, 2 + 9 + choices.size() * 8 + 8);
        data << uint16(1);
        PutEntry(data, ObjectGuid::Create<HighGuid::Player>(low).GetRawValue(), &choices, form);
        return data;
    }

    // Playerbots: a random bot without stored choices gets one random choice per option of its race, sex and class,
    // saved like a player's and shown to everyone nearby.
    void RollBotLook(Player* player)
    {
        if (!s_botLooks || !IsRandomBot(player))
            return;
        ObjectGuid::LowType const low = player->GetGUID().GetCounter();
        Choices choices;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_stored.count(low))
                return;
            auto const pool = s_botPool.find(PoolKey(player->getRace(), player->getGender(), player->getClass()));
            if (pool == s_botPool.end())
                return;
            for (auto const& [option, list] : pool->second)
                if (!list.empty())
                    choices[option] = list[urand(0, uint32(list.size() - 1))];
        }
        if (choices.empty())
            return;
        Save(low, player->GetSession()->GetAccountId(), choices);
        WorldPacket data = ChoicesPacket(low, choices);
        player->SendMessageToSet(&data, true);
        LOG_DEBUG("module", "Retail customization: bot {} given {} choices", player->GetName(), choices.size());
    }

    // All stored choices of the account's characters, and the form of those in their altered form, for the
    // character select screen.
    void SendAccountChoices(WorldSession* session)
    {
        std::map<ObjectGuid::LowType, std::pair<Choices, int>> entries;
        std::map<ObjectGuid::LowType, Forms> others;     // Two Forms characters' other form, for the select screen
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            for (auto const& [low, stored] : s_stored)
                if (stored.account == session->GetAccountId())
                    entries.emplace(low, std::make_pair(stored.choices, -1));
            // Every Two Forms character's form, 0 included, so a client never keeps one from an earlier list.
            for (auto const& [low, forms] : s_forms)
                if (forms.account == session->GetAccountId())
                {
                    entries.emplace(low, std::make_pair(Choices(), -1)).first->second.second = forms.form;
                    others[low] = forms;
                }
        }
        WorldPacket data(kChoicesOpcode, 2 + entries.size() * 32);
        data << uint16(entries.size());
        for (auto const& [low, entry] : entries)
        {
            auto const other = others.find(low);
            PutEntry(data, ObjectGuid::Create<HighGuid::Player>(low).GetRawValue(), &entry.first, entry.second,
                other != others.end() ? &other->second : nullptr);
        }
        session->SendPacket(&data);
    }

    // Two Forms: swaps the active form's look with the parked one. `toNative` only goes back to the race's own
    // form. False when the race has no altered form or nothing changes.
    bool ChangeForm(Player* player, bool toNative)
    {
        if (!AlteredFormRace(player->getRace()))
            return false;
        ObjectGuid::LowType const low = player->GetGUID().GetCounter();
        uint8 current[5] = {
            player->GetByteValue(PLAYER_BYTES, 0), player->GetByteValue(PLAYER_BYTES, 1),
            player->GetByteValue(PLAYER_BYTES, 2), player->GetByteValue(PLAYER_BYTES, 3),
            player->GetByteValue(PLAYER_BYTES_2, 0),
        };
        uint8 next[5];
        uint8 form;
        Choices active, parked;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            Forms& forms = s_forms[low];
            if (toNative && !forms.form)
                return false;
            forms.account = player->GetSession()->GetAccountId();
            Stored& stored = s_stored[low];
            stored.account = forms.account;
            std::copy(std::begin(forms.parked), std::end(forms.parked), next);
            std::copy(std::begin(current), std::end(current), forms.parked);
            std::swap(stored.choices, forms.parkedChoices);
            forms.form ^= 1;
            form = forms.form;
            active = stored.choices;
            parked = forms.parkedChoices;
        }

        player->SetByteValue(PLAYER_BYTES, 0, next[0]);
        player->SetByteValue(PLAYER_BYTES, 1, next[1]);
        player->SetByteValue(PLAYER_BYTES, 2, next[2]);
        player->SetByteValue(PLAYER_BYTES, 3, next[3]);
        player->SetByteValue(PLAYER_BYTES_2, 0, next[4]);
        if (form)
            player->SetFlag(UNIT_FIELD_FLAGS_2, UNIT_FLAG2_ALTERED_FORM);
        else
            player->RemoveFlag(UNIT_FIELD_FLAGS_2, UNIT_FLAG2_ALTERED_FORM);

        // Both looks and the character row in one transaction, so a crash never leaves them mixed.
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append("REPLACE INTO `character_two_forms` (`guid`, `form`, `skin`, `face`, `hair_style`, `hair_color`, `facial_hair`) "
            "VALUES ({}, {}, {}, {}, {}, {}, {})", low, form, current[0], current[1], current[2], current[3], current[4]);
        trans->Append("UPDATE `characters` SET `skin` = {}, `face` = {}, `hairStyle` = {}, `hairColor` = {}, `facialStyle` = {} WHERE `guid` = {}",
            next[0], next[1], next[2], next[3], next[4], low);
        trans->Append("DELETE FROM `character_two_forms_choices` WHERE `guid` = {}", low);
        for (auto const& [option, choice] : parked)
            trans->Append("INSERT INTO `character_two_forms_choices` (`guid`, `option_id`, `choice_id`) VALUES ({}, {}, {})", low, option, choice);
        trans->Append("DELETE FROM `character_retail_customization` WHERE `guid` = {}", low);
        for (auto const& [option, choice] : active)
            trans->Append("INSERT INTO `character_retail_customization` (`guid`, `option_id`, `choice_id`) VALUES ({}, {}, {})", low, option, choice);
        CharacterDatabase.CommitTransaction(trans);

        WorldPacket data = ChoicesPacket(low, active, form);
        player->SendMessageToSet(&data, true);
        LOG_INFO("module", "Two Forms: {} is now in {} form", player->GetName(), form ? "its altered" : "its own");
        return true;
    }

    void AnswerQuery(WorldSession* session, WorldPacket packet)
    {
        packet.rpos(0);
        if (packet.size() < 1)
            return;
        uint8 count = 0;
        packet >> count;
        if (count > kMaxQuery)
            count = kMaxQuery;
        count = uint8(std::min<size_t>(count, (packet.size() - packet.rpos()) / 8));

        WorldPacket data(kChoicesOpcode, 2 + count * 32);
        data << uint16(count);
        std::lock_guard<std::mutex> lock(s_mutex);
        for (uint8 i = 0; i < count; ++i)
        {
            uint64 raw = 0;
            packet >> raw;
            ObjectGuid guid(raw);
            auto stored = guid.IsPlayer() ? s_stored.find(guid.GetCounter()) : s_stored.end();
            PutEntry(data, raw, stored != s_stored.end() ? &stored->second.choices : nullptr);
        }
        session->SendPacket(&data);
    }

    // CMSG_CHAR_CREATE: name, race, class, gender, skin, face, hair style, hair colour, facial hair,
    // outfit; then the block. Kept per account until the character is committed (OnPlayerCreate).
    void ReadCreate(WorldSession* session, WorldPacket packet)
    {
        packet.rpos(0);
        std::string name;
        packet >> name;
        packet.read_skip(9);
        size_t const trailing = packet.size() - packet.rpos();
        uint32 const account = session->GetAccountId();
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_pendingForms.erase(account);
        }
        Choices choices;
        size_t const start = packet.rpos();
        if (!ReadBlock(packet, choices))
        {
            packet.rpos(start);
            LOG_INFO("module", "Retail customization: create '{}' (account {}): {} bytes, {} after the stock fields, no RTLC block",
                name, account, packet.size(), trailing);
        }
        else
        {
            LOG_INFO("module", "Retail customization: create '{}' (account {}): {} choices received", name, account, choices.size());
            std::lock_guard<std::mutex> lock(s_mutex);
            s_pendingCreate[account] = std::move(choices);
        }

        // Two Forms: the altered form's look, when the player edited it on the create screen.
        if (packet.size() - packet.rpos() < 10)
            return;
        uint32 magic = 0;
        packet >> magic;
        if (magic != kFormMagic)
            return;
        Forms forms;
        forms.account = account;
        for (uint8 i = 0; i < 5; ++i)
            packet >> forms.parked[i];
        uint8 count = 0;
        packet >> count;
        if (count > kMaxChoices || packet.size() - packet.rpos() < size_t(count) * 8)
            return;
        for (uint8 i = 0; i < count; ++i)
        {
            uint32 option = 0, choice = 0;
            packet >> option >> choice;
            if (option && choice)
                forms.parkedChoices[option] = choice;
        }
        LOG_INFO("module", "Two Forms: create '{}' (account {}): the other form's look received ({} choices)", name, account, forms.parkedChoices.size());
        std::lock_guard<std::mutex> lock(s_mutex);
        s_pendingForms[account] = std::move(forms);
    }

    // CMSG_ALTER_APPEARANCE: hair, colour, facial hair, skin (u32 each); then the block. Applied
    // when the barber shop reports success.
    void ReadBarber(WorldSession* session, WorldPacket packet)
    {
        Player* player = session->GetPlayer();
        if (!player)
            return;
        packet.rpos(0);
        packet.read_skip(16);
        Choices choices;
        std::lock_guard<std::mutex> lock(s_mutex);
        if (ReadBlock(packet, choices))
            s_pendingBarber[player->GetGUID().GetCounter()] = std::move(choices);
        else
            s_pendingBarber.erase(player->GetGUID().GetCounter());
    }
}

class RetailCustomizationWorld : public WorldScript
{
public:
    RetailCustomizationWorld() : WorldScript("RetailCustomizationWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        s_enabled = sConfigMgr->GetOption<bool>("RetailCustomization.Enable", true);
        s_botLooks = sConfigMgr->GetOption<bool>("RetailCustomization.BotLooks", true);
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_botAccountPrefix = sConfigMgr->GetOption<std::string>("RetailCustomization.BotAccountPrefix", "RNDBOT");
            s_botAccounts.clear();
        }
        s_toggleSpells.clear();
        std::string const toggleSpells = sConfigMgr->GetOption<std::string>("TwoForms.ToggleSpell", "");
        for (std::string_view id : Acore::Tokenize(toggleSpells, ',', false))
            if (Optional<uint32> spell = Acore::StringTo<uint32>(id); spell && *spell)
                s_toggleSpells.insert(*spell);
        s_nativeFormInCombat = sConfigMgr->GetOption<bool>("TwoForms.NativeFormInCombat", true);
        s_nativeFormInCombatRaces.clear();
        std::string const combatRaces = sConfigMgr->GetOption<std::string>("TwoForms.NativeFormInCombatRaces", "16");
        for (std::string_view id : Acore::Tokenize(combatRaces, ',', false))
            if (Optional<uint32> race = Acore::StringTo<uint32>(id))
                s_nativeFormInCombatRaces.insert(*race);
        s_raceFormSpells.clear();
        std::string const raceSpells = sConfigMgr->GetOption<std::string>("TwoForms.RaceSpells", "");
        for (std::string_view pair : Acore::Tokenize(raceSpells, ',', false))
        {
            std::vector<std::string_view> parts = Acore::Tokenize(pair, ':', false);
            if (parts.size() != 2)
                continue;
            Optional<uint32> race = Acore::StringTo<uint32>(parts[0]);
            Optional<uint32> spell = Acore::StringTo<uint32>(parts[1]);
            if (race && spell && *spell)
                s_raceFormSpells[*race] = *spell;
        }
        s_nativeFormSpells.clear();
        std::string const nativeFormSpells = sConfigMgr->GetOption<std::string>("TwoForms.NativeFormSpells", "");
        for (std::string_view id : Acore::Tokenize(nativeFormSpells, ',', false))
            if (Optional<uint32> spell = Acore::StringTo<uint32>(id))
                s_nativeFormSpells.insert(*spell);
    }

    void OnStartup() override
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_stored.clear();
        QueryResult result = CharacterDatabase.Query(
            "SELECT r.`guid`, c.`account`, r.`option_id`, r.`choice_id` FROM `character_retail_customization` r "
            "JOIN `characters` c ON c.`guid` = r.`guid`");
        uint32 rows = 0;
        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                Stored& stored = s_stored[fields[0].Get<uint32>()];
                stored.account = fields[1].Get<uint32>();
                stored.choices[fields[2].Get<uint32>()] = fields[3].Get<uint32>();
                ++rows;
            } while (result->NextRow());
        }
        LOG_INFO("module", ">> Retail customization: {} choices of {} characters loaded", rows, s_stored.size());

        s_forms.clear();
        result = CharacterDatabase.Query(
            "SELECT f.`guid`, c.`account`, f.`form`, f.`skin`, f.`face`, f.`hair_style`, f.`hair_color`, f.`facial_hair` "
            "FROM `character_two_forms` f JOIN `characters` c ON c.`guid` = f.`guid`");
        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                Forms& forms = s_forms[fields[0].Get<uint32>()];
                forms.account = fields[1].Get<uint32>();
                forms.form = fields[2].Get<uint8>();
                for (uint8 i = 0; i < 5; ++i)
                    forms.parked[i] = fields[3 + i].Get<uint8>();
            } while (result->NextRow());
        }
        result = CharacterDatabase.Query("SELECT `guid`, `option_id`, `choice_id` FROM `character_two_forms_choices`");
        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                auto forms = s_forms.find(fields[0].Get<uint32>());
                if (forms != s_forms.end())
                    forms->second.parkedChoices[fields[1].Get<uint32>()] = fields[2].Get<uint32>();
            } while (result->NextRow());
        }
        LOG_INFO("module", ">> Two Forms: {} characters with a second form loaded", s_forms.size());

        s_botPool.clear();
        result = WorldDatabase.Query("SELECT `race`, `sex`, `class`, `option_id`, `choice_id` FROM `retail_customization_bot_pool` "
            "ORDER BY `race`, `sex`, `class`, `option_id`");
        rows = 0;
        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                Pool& pool = s_botPool[PoolKey(fields[0].Get<uint8>(), fields[1].Get<uint8>(), fields[2].Get<uint8>())];
                uint32 const option = fields[3].Get<uint32>();
                if (pool.empty() || pool.back().first != option)
                    pool.emplace_back(option, std::vector<uint32>());
                pool.back().second.push_back(fields[4].Get<uint32>());
                ++rows;
            } while (result->NextRow());
        }
        LOG_INFO("module", ">> Retail customization: {} bot choices for {} race, sex and class combinations loaded", rows, s_botPool.size());
    }
};

class RetailCustomizationServer : public ServerScript
{
public:
    RetailCustomizationServer() : ServerScript("RetailCustomizationServer", { SERVERHOOK_CAN_PACKET_RECEIVE, SERVERHOOK_CAN_PACKET_SEND }) { }

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        if (!s_enabled || !session)
            return true;
        try
        {
            switch (packet.GetOpcode())
            {
                case CMSG_CHAR_CREATE:
                    ReadCreate(session, packet);
                    return true;
                case CMSG_CHAR_ENUM:
                    SendAccountChoices(session);    // arrives before the character list it belongs to
                    return true;
                case CMSG_ALTER_APPEARANCE:
                    ReadBarber(session, packet);
                    return true;
                case kQueryOpcode:
                    AnswerQuery(session, packet);
                    return false;                   // no stock handler (it would log "unhandled opcode")
                default:
                    return true;
            }
        }
        catch (ByteBufferException const&)
        {
            return packet.GetOpcode() != kQueryOpcode;
        }
    }

    bool CanPacketSend(WorldSession* session, WorldPacket const& packet) override
    {
        if (!s_enabled || !session || packet.GetOpcode() != SMSG_BARBER_SHOP_RESULT || packet.size() < 4)
            return true;
        Player* player = session->GetPlayer();
        if (!player)
            return true;
        uint32 result = packet.read<uint32>(0);
        Choices choices;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            auto pending = s_pendingBarber.find(player->GetGUID().GetCounter());
            if (pending == s_pendingBarber.end())
                return true;
            choices = std::move(pending->second);
            s_pendingBarber.erase(pending);
        }
        if (result == 0)    // BARBER_SHOP_RESULT_SUCCESS
        {
            Save(player->GetGUID().GetCounter(), session->GetAccountId(), choices);
            WorldPacket data = ChoicesPacket(player->GetGUID().GetCounter(), choices);
            player->SendMessageToSet(&data, true);
        }
        return true;
    }
};

class RetailCustomizationPlayer : public PlayerScript
{
public:
    RetailCustomizationPlayer() : PlayerScript("RetailCustomizationPlayer", { PLAYERHOOK_ON_CREATE, PLAYERHOOK_ON_DELETE,
        PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_SPELL_CAST, PLAYERHOOK_ON_PLAYER_ENTER_COMBAT }) { }

    // Two Forms: entering combat returns a player to the race's own form (a Worgen fights as a wolf).
    void OnPlayerEnterCombat(Player* player, Unit* /*enemy*/) override
    {
        if (s_enabled && s_nativeFormInCombat && s_nativeFormInCombatRaces.count(player->getRace()) &&
            player->HasFlag(UNIT_FIELD_FLAGS_2, UNIT_FLAG2_ALTERED_FORM))
            ChangeForm(player, true);
    }

    // Two Forms: a new character keeps the other form's look from the create screen; it starts in its own form.
    void CreateForms(Player* player, uint32 account)
    {
        Forms forms;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            auto pending = s_pendingForms.find(account);
            if (pending == s_pendingForms.end())
                return;
            forms = std::move(pending->second);
            s_pendingForms.erase(pending);
            if (!AlteredFormRace(player->getRace()))
                return;
            s_forms[player->GetGUID().GetCounter()] = forms;
        }
        ObjectGuid::LowType const low = player->GetGUID().GetCounter();
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append("REPLACE INTO `character_two_forms` (`guid`, `form`, `skin`, `face`, `hair_style`, `hair_color`, `facial_hair`) "
            "VALUES ({}, 0, {}, {}, {}, {}, {})", low, forms.parked[0], forms.parked[1], forms.parked[2], forms.parked[3], forms.parked[4]);
        trans->Append("DELETE FROM `character_two_forms_choices` WHERE `guid` = {}", low);
        for (auto const& [option, choice] : forms.parkedChoices)
            trans->Append("INSERT INTO `character_two_forms_choices` (`guid`, `option_id`, `choice_id`) VALUES ({}, {}, {})", low, option, choice);
        CharacterDatabase.CommitTransaction(trans);
        LOG_INFO("module", "Two Forms: {} saved with its other form's look", player->GetName());
    }

    // Playerbots: a random bot's first login gives it a look. Two Forms: a character saved in its altered form
    // logs in showing it.
    void OnPlayerLogin(Player* player) override
    {
        if (!s_enabled)
            return;
        RollBotLook(player);
        if (!AlteredFormRace(player->getRace()))
            return;
        auto const known = s_raceFormSpells.find(player->getRace());
        if (known != s_raceFormSpells.end() && !player->HasSpell(known->second) && sSpellMgr->GetSpellInfo(known->second))
            player->learnSpell(known->second);
        bool altered = false;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            auto forms = s_forms.find(player->GetGUID().GetCounter());
            altered = forms != s_forms.end() && forms->second.form;
        }
        if (altered)
            player->SetFlag(UNIT_FIELD_FLAGS_2, UNIT_FLAG2_ALTERED_FORM);
        else
            player->RemoveFlag(UNIT_FIELD_FLAGS_2, UNIT_FLAG2_ALTERED_FORM);
    }

    // Two Forms: the toggle spell changes form (not in combat); Darkflight-like spells return to the race's own form.
    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (!s_enabled || !spell || !spell->GetSpellInfo() || !AlteredFormRace(player->getRace()))
            return;
        uint32 const id = spell->GetSpellInfo()->Id;
        if (s_toggleSpells.count(id))
        {
            if (player->IsInCombat())
            {
                ChatHandler(player->GetSession()).SendNotification("You can't change form while in combat.");
                return;
            }
            ChangeForm(player, false);
        }
        else if (s_nativeFormSpells.count(id))
            ChangeForm(player, true);
    }

    void OnPlayerCreate(Player* player) override
    {
        if (!s_enabled || !player->GetSession())
            return;
        uint32 account = player->GetSession()->GetAccountId();
        CreateForms(player, account);
        Choices choices;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            auto pending = s_pendingCreate.find(account);
            if (pending == s_pendingCreate.end())
            {
                LOG_INFO("module", "Retail customization: {} created with no choices pending for account {}", player->GetName(), account);
                return;
            }
            choices = std::move(pending->second);
            s_pendingCreate.erase(pending);
        }
        Save(player->GetGUID().GetCounter(), account, choices);
        LOG_INFO("module", "Retail customization: {} choices saved for {}", choices.size(), player->GetName());
    }

    void OnPlayerDelete(ObjectGuid guid, uint32 /*accountId*/) override
    {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_stored.erase(guid.GetCounter());
            s_pendingBarber.erase(guid.GetCounter());
            s_forms.erase(guid.GetCounter());
        }
        CharacterDatabase.Execute("DELETE FROM `character_retail_customization` WHERE `guid` = {}", guid.GetCounter());
        CharacterDatabase.Execute("DELETE FROM `character_two_forms` WHERE `guid` = {}", guid.GetCounter());
        CharacterDatabase.Execute("DELETE FROM `character_two_forms_choices` WHERE `guid` = {}", guid.GetCounter());
    }
};

void AddSC_RetailCustomization()
{
    new RetailCustomizationWorld();
    new RetailCustomizationServer();
    new RetailCustomizationPlayer();
}
