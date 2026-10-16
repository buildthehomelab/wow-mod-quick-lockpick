/*
 * mod-quick-lockpick: open a lockbox in one step.
 *
 * In stock 3.3.5 a rogue opens a lockbox in three steps: cast Pick Lock, click the box, then
 * right-click the box again to see what's inside. This module cuts that down to one right-click:
 *
 *   right-click a locked box  the server hook below casts Pick Lock on it, as if the rogue had
 *                             cast the spell and clicked the box (QuickLockpick.RightClickPicks)
 *   the box comes unlocked    the spell hook below opens it, as a right-click would
 *                             (QuickLockpick.OpenAfterUnlock)
 *
 * The second part works for every way of unlocking an item in your own bags: Pick Lock cast by
 * hand or a skeleton key. Casting Pick Lock from the spellbook and clicking the
 * box still works, and also opens it right away.
 *
 * The client sends a right-click on a box as CMSG_OPEN_ITEM, and the core answers a locked one
 * with "Item is locked". The packet hook runs just before that handler, wherever the core
 * processes the packet (the player's map update or the world session update; nothing else
 * touches the player during either), so it can cast in its place. Pick Lock goes through every
 * normal check: the cast time, moving interrupts it, and a box above the rogue's lockpicking
 * skill fails with the usual message. A right-click while something else is being cast gets
 * the stock answer rather than interrupting it. Bots are left alone; playerbots opens boxes
 * its own way.
 *
 * The client grays an item out when it sends CMSG_OPEN_ITEM and only gives it back on a loot
 * response or a loot release for it. A right-click Pick Lock that doesn't end in the loot window
 * (it failed, was interrupted, or OpenAfterUnlock is off) sends the release.
 *
 * Released under the MIT License.
 */

#include "Config.h"
#include "DataMap.h"
#include "DBCStores.h"
#include "Item.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <type_traits>
#include <utility>

namespace
{
    constexpr uint32 SPELL_PICK_LOCK = 1804;

    struct Config
    {
        bool enabled = true;
        bool rightClickPicks = true;
        bool openAfterUnlock = true;
    };

    Config config;

    // The box a right-click Pick Lock is under way on, so only that cast un-grays it afterwards.
    struct PendingPick : public DataMap::Base
    {
        ObjectGuid box;
    };

    PendingPick* GetPendingPick(Player* player)
    {
        return player->CustomData.GetDefault<PendingPick>("mod-quick-lockpick");
    }

    // Ends a right-click Pick Lock on this box. Without the loot window the client needs a release.
    void FinishPendingPick(Player* player, ObjectGuid box, bool opened)
    {
        PendingPick* pending = GetPendingPick(player);
        if (!box || pending->box != box)
            return;

        pending->box.Clear();
        if (!opened)
            player->SendLootRelease(box);
    }

    // Bot sessions: IsHeadless() on current playerbots cores, IsBot() on older ones.
    template <typename Session, typename = void>
    struct HasIsHeadless : std::false_type { };

    template <typename Session>
    struct HasIsHeadless<Session, std::void_t<decltype(std::declval<Session&>().IsHeadless())>> : std::true_type { };

    template <typename Session, typename = void>
    struct HasIsBot : std::false_type { };

    template <typename Session>
    struct HasIsBot<Session, std::void_t<decltype(std::declval<Session&>().IsBot())>> : std::true_type { };

    template <typename Session>
    bool IsBotSession(Session* session)
    {
        if constexpr (HasIsHeadless<Session>::value)
            return session->IsHeadless();
        else if constexpr (HasIsBot<Session>::value)
            return session->IsBot();
        else
            return false;
    }

    bool IsRealPlayer(Player* player)
    {
        WorldSession* session = player->GetSession();
        return session && !IsBotSession(session);
    }

    // Backpack and bags, not the bank.
    bool IsInBags(Item const* item)
    {
        return Player::IsInventoryPos(item->GetBagSlot(), item->GetSlot());
    }

    // A box with loot that's still locked. Wrapped gifts and other openable items don't count.
    bool IsLockedBox(Item const* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        return proto && proto->LockID && proto->HasFlag(ITEM_FLAG_HAS_LOOT) && !item->IsWrapped() && item->IsLocked();
    }

    // Whether Pick Lock is one of the ways to open this lock. Boxes that need a key item, or
    // a lock type other than lockpicking, keep the stock "Item is locked".
    bool IsPickable(uint32 lockId)
    {
        LockEntry const* lock = sLockStore.LookupEntry(lockId);
        if (!lock)
            return false;

        for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
            if (lock->Type[i] == LOCK_KEY_SKILL && lock->Index[i] == LOCKTYPE_PICKLOCK)
                return true;

        return false;
    }

    // What the core's CMSG_OPEN_ITEM handler does with an unlocked box, minus the checks the
    // caller has made. Other modules' OnPlayerBeforeOpenItem hooks get their say.
    bool OpenBox(Player* player, Item* item)
    {
        if (!player->IsAlive() || !sScriptMgr->OnPlayerBeforeOpenItem(player, item))
            return false;

        player->SendLoot(item->GetGUID(), LOOT_CORPSE);
        return true;
    }

    // The item a Pick Lock (or key) cast is aimed at, if it's a box in the caster's own bags.
    // Looked up again by GUID: the spell's raw item pointer isn't refreshed on every path and
    // can outlive an item destroyed mid-cast. Trade-slot targets carry no item flag, so a box
    // someone else put in the trade window is skipped.
    Item* OwnBoxTarget(Spell* spell, Player* player)
    {
        if (!(spell->m_targets.GetTargetMask() & TARGET_FLAG_ITEM))
            return nullptr;

        Item* item = player->GetItemByGuid(spell->m_targets.GetItemTargetGUID());
        if (!item || item->IsInTrade() || !IsInBags(item))
            return nullptr;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || !proto->LockID || !proto->HasFlag(ITEM_FLAG_HAS_LOOT) || item->IsWrapped())
            return nullptr;

        return item;
    }
}

class QuickLockpickServerScript : public ServerScript
{
public:
    QuickLockpickServerScript() : ServerScript("QuickLockpickServerScript", { SERVERHOOK_CAN_PACKET_RECEIVE }) { }

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        if (!config.enabled || !config.rightClickPicks || packet.GetOpcode() != CMSG_OPEN_ITEM || packet.size() < 2)
            return true;

        Player* player = session ? session->GetPlayer() : nullptr;
        if (!player || !player->IsInWorld() || !player->IsAlive() || player->m_mover != player || !IsRealPlayer(player))
            return true;

        // Something else is being cast (Hearthstone, a craft, Pick Lock itself): leave it be.
        if (!player->HasSpell(SPELL_PICK_LOCK) || player->IsNonMeleeSpellCast(false, false, true))
            return true;

        // CMSG_OPEN_ITEM is bag, slot. Read in place; the core's handler reads the packet itself.
        uint8 bag = packet.read<uint8>(0);
        uint8 slot = packet.read<uint8>(1);

        Item* item = player->GetItemByPos(bag, slot);
        if (!item || item->IsInTrade() || !IsInBags(item) || !IsLockedBox(item) || !IsPickable(item->GetTemplate()->LockID))
            return true;

        SpellInfo const* pickLock = sSpellMgr->GetSpellInfo(SPELL_PICK_LOCK);
        if (!pickLock)
            return true;

        SpellCastTargets targets;
        targets.SetItemTarget(item);

        // A failed cast already shows its reason ("Skill not high enough", "Can't do that while
        // moving"); give the grayed-out box back. A cast that starts is finished by the spell
        // hooks below.
        // Marked before the cast: one with no cast time finishes inside CastSpell.
        ObjectGuid box = item->GetGUID();
        PendingPick* pending = GetPendingPick(player);
        pending->box = box;
        if (player->CastSpell(targets, pickLock, nullptr, TRIGGERED_NONE) != SPELL_CAST_OK)
        {
            pending->box.Clear();
            player->SendLootRelease(box);
        }

        return false;
    }
};

class QuickLockpickSpellScript : public AllSpellScript
{
public:
    QuickLockpickSpellScript() : AllSpellScript("QuickLockpickSpellScript", { ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_CAST_CANCEL }) { }

    // Runs after the cast's effects, so a box Pick Lock or a key just opened is unlocked by now.
    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        if (!caster || !spellInfo->HasEffect(SPELL_EFFECT_OPEN_LOCK))
            return;

        Player* player = caster->ToPlayer();
        if (!player || !IsRealPlayer(player))
            return;

        // Boxes in a trade window belong to the other player; they open it themselves.
        bool opened = false;
        if (config.enabled && config.openAfterUnlock)
            if (Item* item = OwnBoxTarget(spell, player))
                if (!item->IsLocked())
                    opened = OpenBox(player, item);

        FinishPendingPick(player, spell->m_targets.GetItemTargetGUID(), opened);
    }

    // A right-click Pick Lock that was interrupted: give the grayed-out box back to the client.
    // Only the GUID is used here; the item may already be gone.
    void OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool /*bySelf*/) override
    {
        if (!caster || spellInfo->Id != SPELL_PICK_LOCK)
            return;

        if (Player* player = caster->ToPlayer())
            if (IsRealPlayer(player))
                FinishPendingPick(player, spell->m_targets.GetItemTargetGUID(), false);
    }
};

class QuickLockpickWorldScript : public WorldScript
{
public:
    QuickLockpickWorldScript() : WorldScript("QuickLockpickWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        config.enabled = sConfigMgr->GetOption<bool>("QuickLockpick.Enable", true);
        config.rightClickPicks = sConfigMgr->GetOption<bool>("QuickLockpick.RightClickPicks", true);
        config.openAfterUnlock = sConfigMgr->GetOption<bool>("QuickLockpick.OpenAfterUnlock", true);
    }
};

void AddQuickLockpickScripts()
{
    new QuickLockpickServerScript();
    new QuickLockpickSpellScript();
    new QuickLockpickWorldScript();
}
