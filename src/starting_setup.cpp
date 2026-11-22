/*
 * wow-tofuworld-starting-setup (Tofu Worldview)
 *
 * - Starting kit, once per character: weapon proficiencies, gold, random bag(s), items, welcome mail.
 *   New characters get it on first login; existing characters only when StartingSetup.ExistingCharacters = 1 (by mail).
 *   Random bots and addclass bots never get it; altbots (player-made characters) do.
 * - The kit record is keyed by character GUID. The core reuses the GUID of a deleted newest character after a restart
 *   (next GUID = MAX(characters.guid) + 1), so the record is deleted with the character, orphan records are purged at
 *   startup, and a record older than the character's creation date is ignored.
 * - PvP on by default (StartingSetup.PvPDefaultOn): a new player-made character starts with /pvp on, once, at its first login.
 *   The core saves PLAYER_FLAGS_IN_PVP in characters.playerFlags and restores it at login, so a player's later /pvp off sticks.
 *   StartingSetup.PvPDefaultOn.RandomBots flags random bots at every login (bots never turn it off themselves).
 * - Every character joins the configured channel (default "World") at every login.
 *   The client's own rejoin then triggers an "already on the channel" notice, which is dropped.
 *
 * Written against mod-playerbots/azerothcore-wotlk 32fd347 and mod-playerbots ecc4a53.
 */

#include "Channel.h"
#include "ChannelMgr.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // Weapon proficiency spells, checked against the server's Spell.dbc and SkillLineAbility.dbc (2026-11-18).
    // Dual Wield (674) is deliberately not included.
    constexpr std::array<uint32, 15> WeaponSpells =
    {
        201,   // One-Handed Swords
        202,   // Two-Handed Swords
        196,   // One-Handed Axes
        197,   // Two-Handed Axes
        198,   // One-Handed Maces
        199,   // Two-Handed Maces
        200,   // Polearms
        227,   // Staves
        1180,  // Daggers
        15590, // Fist Weapons
        264,   // Bows
        266,   // Guns
        5011,  // Crossbows
        2567,  // Thrown
        5009   // Wands
    };

    // A join sent during login is dropped by the client (the server counts the player as a member, the client does not), so a player's first-login join waits until the client has loaded.
    constexpr Milliseconds AutoJoinDelay = Milliseconds(3000);

    using ItemList = std::vector<std::pair<uint32, uint32>>; // item entry, count

    struct Settings
    {
        bool enable = true;
        bool announce = true;
        bool existingCharacters = false;
        uint32 weaponSkills = 1;
        uint32 gold = 1000;
        std::vector<uint32> bagIds;
        uint32 bagCount = 1;
        ItemList items;
        std::array<ItemList, MAX_CLASSES> classItems;
        std::string kitMailSubject;
        std::string kitMailBody;
        bool mailEnable = false;
        uint32 mailSender = 0;
        std::string mailSubject;
        std::string mailBody;
        uint32 mailGold = 0;
        ItemList mailItems;
        bool pvpDefaultOn = false;
        bool pvpRandomBots = false;
        bool autoJoinEnable = true;
        std::string autoJoinChannel = "World";
    };

    Settings settings;

    // "id:count,id:count" or "id,id" (count 1). Malformed entries are logged and skipped; unknown items are removed by ValidateSettings.
    ItemList ParseItemList(std::string const& key, std::string const& value)
    {
        ItemList result;
        for (std::string_view token : Acore::Tokenize(value, ',', false))
        {
            std::vector<std::string_view> parts = Acore::Tokenize(token, ':', false);
            if (parts.empty() || parts.size() > 2)
            {
                LOG_ERROR("module", "StartingSetup: {} has a malformed entry '{}', skipped.", key, token);
                continue;
            }

            Optional<uint32> entry = Acore::StringTo<uint32>(parts[0]);
            Optional<uint32> count = parts.size() == 2 ? Acore::StringTo<uint32>(parts[1]) : Optional<uint32>(1);
            if (!entry || !count || !*entry || !*count)
            {
                LOG_ERROR("module", "StartingSetup: {} has a malformed entry '{}', skipped.", key, token);
                continue;
            }

            result.emplace_back(*entry, *count);
        }
        return result;
    }

    void LoadSettings()
    {
        Settings s;
        s.enable = sConfigMgr->GetOption<bool>("StartingSetup.Enable", true);
        s.announce = sConfigMgr->GetOption<bool>("StartingSetup.Announce", true);
        s.existingCharacters = sConfigMgr->GetOption<bool>("StartingSetup.ExistingCharacters", false);
        s.weaponSkills = sConfigMgr->GetOption<uint32>("StartingSetup.WeaponSkills", 1);
        if (s.weaponSkills > 2)
        {
            LOG_ERROR("module", "StartingSetup: StartingSetup.WeaponSkills = {} is not 0, 1, or 2; using 1.", s.weaponSkills);
            s.weaponSkills = 1;
        }
        s.gold = sConfigMgr->GetOption<uint32>("StartingSetup.Gold", 1000);

        for (auto const& [entry, count] : ParseItemList("StartingSetup.Bags.Ids", sConfigMgr->GetOption<std::string>("StartingSetup.Bags.Ids", "4496,5572,805,828,5571")))
            s.bagIds.push_back(entry);
        s.bagCount = std::min<uint32>(sConfigMgr->GetOption<uint32>("StartingSetup.Bags.Count", 1), 4);

        s.items = ParseItemList("StartingSetup.Items", sConfigMgr->GetOption<std::string>("StartingSetup.Items", ""));
        for (uint8 cls = 1; cls < MAX_CLASSES; ++cls)
        {
            if (cls == 10) // no class 10 in 3.3.5a
                continue;
            std::string key = "StartingSetup.Items.Class." + std::to_string(cls);
            s.classItems[cls] = ParseItemList(key, sConfigMgr->GetOption<std::string>(key, "", false));
        }

        s.kitMailSubject = sConfigMgr->GetOption<std::string>("StartingSetup.KitMail.Subject", "Your starting kit");
        s.kitMailBody = sConfigMgr->GetOption<std::string>("StartingSetup.KitMail.Body", "Welcome to Tofu Worldview. Here is your starting kit.");

        s.mailEnable = sConfigMgr->GetOption<bool>("StartingSetup.Mail.Enable", false);
        s.mailSender = sConfigMgr->GetOption<uint32>("StartingSetup.Mail.Sender", 0);
        s.mailSubject = sConfigMgr->GetOption<std::string>("StartingSetup.Mail.Subject", "");
        s.mailBody = sConfigMgr->GetOption<std::string>("StartingSetup.Mail.Body", "");
        s.mailGold = sConfigMgr->GetOption<uint32>("StartingSetup.Mail.Gold", 0);
        s.mailItems = ParseItemList("StartingSetup.Mail.Items", sConfigMgr->GetOption<std::string>("StartingSetup.Mail.Items", ""));

        s.pvpDefaultOn = sConfigMgr->GetOption<bool>("StartingSetup.PvPDefaultOn", false);
        s.pvpRandomBots = sConfigMgr->GetOption<bool>("StartingSetup.PvPDefaultOn.RandomBots", false);

        s.autoJoinEnable = sConfigMgr->GetOption<bool>("StartingSetup.AutoJoin.Enable", true);
        s.autoJoinChannel = sConfigMgr->GetOption<std::string>("StartingSetup.AutoJoin.Channel", "World");
        if (s.autoJoinChannel.empty())
            s.autoJoinEnable = false;

        settings = std::move(s);
    }

    void FilterItems(char const* key, ItemList& list)
    {
        list.erase(std::remove_if(list.begin(), list.end(), [key](std::pair<uint32, uint32> const& e)
        {
            if (sObjectMgr->GetItemTemplate(e.first))
                return false;
            LOG_ERROR("module", "StartingSetup: {} lists item {}, which does not exist, skipped.", key, e.first);
            return true;
        }), list.end());
    }

    // Item and creature templates are not loaded yet when the config is first read, so this runs at startup and on reload.
    void ValidateSettings()
    {
        settings.bagIds.erase(std::remove_if(settings.bagIds.begin(), settings.bagIds.end(), [](uint32 entry)
        {
            if (sObjectMgr->GetItemTemplate(entry))
                return false;
            LOG_ERROR("module", "StartingSetup: StartingSetup.Bags.Ids lists item {}, which does not exist, skipped.", entry);
            return true;
        }), settings.bagIds.end());

        FilterItems("StartingSetup.Items", settings.items);
        for (uint8 cls = 1; cls < MAX_CLASSES; ++cls)
            FilterItems("StartingSetup.Items.Class.<id>", settings.classItems[cls]);
        FilterItems("StartingSetup.Mail.Items", settings.mailItems);

        if (settings.mailSender && !sObjectMgr->GetCreatureTemplate(settings.mailSender))
        {
            LOG_ERROR("module", "StartingSetup: StartingSetup.Mail.Sender = {} is not a creature entry; using the system sender.", settings.mailSender);
            settings.mailSender = 0;
        }
    }

    bool IsKitExcludedBot(Player* player)
    {
        return sRandomPlayerbotMgr.IsRandomBot(player) || sRandomPlayerbotMgr.IsAddclassBot(player);
    }

    // A record counts only if it was written after this character was created. An older record belongs to a deleted
    // character whose GUID the core has handed out again; it is ignored and replaced when this character gets its kit.
    bool HasKitRecord(Player* player)
    {
        uint32 guid = player->GetGUID().GetCounter();
        QueryResult result = CharacterDatabase.Query("SELECT t.granted_at >= c.creation_date FROM tofu_starting_setup t JOIN characters c ON c.guid = t.guid WHERE t.guid = {}", guid);
        if (!result)
            return false;
        if ((*result)[0].Get<int64>())
            return true;
        LOG_INFO("module", "StartingSetup: character {} (GUID {}) reuses the GUID of a deleted character; its old kit record is ignored.", player->GetName(), guid);
        return false;
    }

    // Removes kit records whose character no longer exists (records left by deletions before this fix).
    void PurgeOrphanRecords()
    {
        QueryResult result = CharacterDatabase.Query("SELECT COUNT(*) FROM tofu_starting_setup t LEFT JOIN characters c ON c.guid = t.guid WHERE c.guid IS NULL");
        uint64 count = result ? (*result)[0].Get<uint64>() : 0;
        if (!count)
            return;
        CharacterDatabase.DirectExecute("DELETE t FROM tofu_starting_setup t LEFT JOIN characters c ON c.guid = t.guid WHERE c.guid IS NULL");
        LOG_INFO("module", "StartingSetup: removed {} kit record(s) of deleted characters.", count);
    }

    // Mode 1: only what the class can normally train (same test trainers use). Mode 2: all of them.
    // Mode 2 skills that the class may not have are deleted by the core at each login, so they are re-learned at each login.
    void ApplyWeaponSkills(Player* player)
    {
        if (settings.weaponSkills == 0)
            return;

        for (uint32 spellId : WeaponSpells)
        {
            if (player->HasSpell(spellId))
                continue;
            if (settings.weaponSkills == 1 && !player->IsSpellFitByClassAndRace(spellId))
                continue;
            player->learnSpell(spellId);
        }
    }

    // Splits each entry into full stacks and creates the items (saved in trans), for mailing.
    std::vector<Item*> CreateMailItems(Player* player, ItemList const& list, CharacterDatabaseTransaction trans)
    {
        std::vector<Item*> result;
        for (auto const& [entry, total] : list)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            if (!proto)
                continue;

            uint32 stackSize = std::max<uint32>(proto->GetMaxStackSize(), 1);
            uint32 remaining = total;
            while (remaining > 0)
            {
                uint32 count = std::min(remaining, stackSize);
                remaining -= count;
                if (Item* item = Item::CreateItem(entry, count, player))
                {
                    item->SaveToDB(trans);
                    result.push_back(item);
                }
            }
        }
        return result;
    }

    // Sends gold and items, 12 items per mail (MAX_MAIL_ITEMS); the gold goes in the first mail.
    void SendMails(Player* player, MailSender const& sender, std::string const& subject, std::string const& body, uint32 money, std::vector<Item*> const& items, CharacterDatabaseTransaction trans)
    {
        if (!money && items.empty())
            return;

        std::size_t index = 0;
        uint32 part = 0;
        do
        {
            ++part;
            std::string partSubject = part == 1 ? subject : subject + " (" + std::to_string(part) + ")";
            MailDraft draft(partSubject, body);
            if (part == 1 && money)
                draft.AddMoney(money);
            for (std::size_t n = 0; n < MAX_MAIL_ITEMS && index < items.size(); ++n, ++index)
                draft.AddItem(items[index]);
            draft.SendMailTo(trans, MailReceiver(player), sender);
        } while (index < items.size());
    }

    MailSender SystemSender(Player* player)
    {
        // Same sender the core's own server mail uses (ServerMailMgr.cpp).
        return MailSender(MAIL_NORMAL, player->GetGUID().GetCounter(), MAIL_STATIONERY_GM);
    }

    MailSender WelcomeSender(Player* player)
    {
        if (settings.mailSender)
            return MailSender(MAIL_CREATURE, settings.mailSender, MAIL_STATIONERY_DEFAULT);
        return SystemSender(player);
    }

    ItemList KitItems(Player* player)
    {
        ItemList list;
        for (uint32 i = 0; i < settings.bagCount && !settings.bagIds.empty(); ++i)
            list.emplace_back(settings.bagIds[urand(0, uint32(settings.bagIds.size() - 1))], 1);
        list.insert(list.end(), settings.items.begin(), settings.items.end());
        uint8 cls = player->getClass();
        if (cls < MAX_CLASSES)
            list.insert(list.end(), settings.classItems[cls].begin(), settings.classItems[cls].end());
        return list;
    }

    void GrantKit(Player* player, bool newCharacter)
    {
        ApplyWeaponSkills(player);

        ItemList kit = KitItems(player);
        ItemList overflow;
        uint32 kitMoney = settings.gold;

        if (newCharacter)
        {
            // Equips bags into bag slots and weapons or armor where possible, else stores in the backpack.
            for (auto const& [entry, count] : kit)
                if (!player->StoreNewItemInBestSlots(entry, count))
                    overflow.emplace_back(entry, count);

            if (kitMoney)
                player->ModifyMoney(int32(kitMoney));
            kitMoney = 0;
        }
        else
            overflow = std::move(kit);

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        SendMails(player, SystemSender(player), settings.kitMailSubject, settings.kitMailBody, kitMoney, CreateMailItems(player, overflow, trans), trans);

        if (settings.mailEnable)
            SendMails(player, WelcomeSender(player), settings.mailSubject, settings.mailBody, settings.mailGold, CreateMailItems(player, settings.mailItems, trans), trans);

        // REPLACE also overwrites an ignored record of a deleted character with the same GUID.
        trans->Append("REPLACE INTO tofu_starting_setup (guid, granted_at) VALUES ({}, CURRENT_TIMESTAMP)", player->GetGUID().GetCounter());
        CharacterDatabase.CommitTransaction(trans);

        // Save the character now so the kit and the record cannot get out of step after a crash.
        if (newCharacter)
            player->SaveToDB(false, false);

        if (settings.announce && player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage(newCharacter ? "|cff00cc44[Starting Setup]|r Your starting kit is ready." : "|cff00cc44[Starting Setup]|r Your starting kit has been mailed to you.");
    }

    // Same as typing /pvp on (WorldSession::HandleTogglePvP). OnPlayerLogin runs after the core's own login restore of the flag, so the state is applied here.
    void TurnPvPOn(Player* player)
    {
        if (player->HasPlayerFlag(PLAYER_FLAGS_IN_PVP))
            return;
        player->SetPlayerFlag(PLAYER_FLAGS_IN_PVP);
        player->UpdatePvP(true, true);
    }

    void JoinChannel(Player* player)
    {
        ChannelMgr* mgr = ChannelMgr::forTeam(player->GetTeamId());
        if (!mgr)
            return;
        if (Channel* channel = mgr->GetJoinChannel(settings.autoJoinChannel, 0))
            channel->JoinChannel(player, "");
    }
}

class StartingSetupWorldScript : public WorldScript
{
public:
    StartingSetupWorldScript() : WorldScript("StartingSetupWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadSettings();
        if (reload)
            ValidateSettings();
    }

    void OnStartup() override
    {
        ValidateSettings();
        PurgeOrphanRecords();
    }
};

class StartingSetupPlayerScript : public PlayerScript
{
public:
    StartingSetupPlayerScript() : PlayerScript("StartingSetupPlayerScript", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_DELETE_FROM_DB }) { }

    // Runs inside the core's character-delete transaction, so the record goes with the character.
    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        trans->Append("DELETE FROM tofu_starting_setup WHERE guid = {}", guid);
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!settings.enable || !player)
            return;

        // Still set here: the core clears it only after OnPlayerLogin (CharacterHandler.cpp).
        bool newCharacter = player->HasAtLoginFlag(AT_LOGIN_FIRST);

        // Bots have no client to remember channels, so they join at every login. A real client saves its channels per character and rejoins them itself, so players join only on a new character's first login (delayed, see AutoJoinDelay), and a manual /leave sticks.
        if (settings.autoJoinEnable)
        {
            bool headless = player->GetSession() && player->GetSession()->IsHeadless();
            if (headless)
                JoinChannel(player);
            else if (newCharacter)
            {
                // The event belongs to the player, so it is dropped if the player logs out first.
                player->m_Events.AddEventAtOffset([player]()
                {
                    if (settings.enable && settings.autoJoinEnable && player->IsInWorld())
                        JoinChannel(player);
                }, AutoJoinDelay);
            }
        }

        if (sRandomPlayerbotMgr.IsRandomBot(player))
        {
            if (settings.pvpRandomBots)
                TurnPvPOn(player);
            return;
        }

        if (IsKitExcludedBot(player))
            return;

        // Once, at the first login only, so a later /pvp off is never overridden.
        if (newCharacter && settings.pvpDefaultOn)
            TurnPvPOn(player);

        if (HasKitRecord(player))
        {
            ApplyWeaponSkills(player); // no-op in mode 1 once learned; re-learns off-class weapons in mode 2
            return;
        }

        if (newCharacter || settings.existingCharacters)
            GrantKit(player, newCharacter);
    }
};

void AddSC_wow_tofuworld_starting_setup()
{
    new StartingSetupWorldScript();
    new StartingSetupPlayerScript();
}
