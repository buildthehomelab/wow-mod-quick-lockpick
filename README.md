# Quick Lockpick

An [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) module that opens lockboxes in one
step. No client patch and no SQL.

## What changes

In stock 3.3.5 a rogue opens a lockbox in three steps: cast Pick Lock, click the box, then
right-click the box again to loot it. With this module:

- **Right-click a locked box** in your bags and you cast Pick Lock on it, if you know Pick Lock.
  A right-click while you're casting something else (Hearthstone, a craft, Pick Lock itself)
  gets the stock "Item is locked" instead of interrupting it.
- **When the lock opens**, the box opens with it and the loot window comes up. With auto loot on,
  the loot goes straight to your bags.

Casting Pick Lock from the spellbook and clicking the box still works, and opens the box right
away too, and so does a skeleton key: any box in your own bags opens as soon as it is unlocked.

Pick Lock still goes through all the normal checks. It has its cast time, moving interrupts it,
and a box above your lockpicking skill fails with the usual message. Boxes that need a key item,
and players without Pick Lock, still get "Item is locked". A box someone puts in the trade window
for you to pick is picked the stock way (cast Pick Lock, click the "Will not be traded" slot). It
goes back to its owner unlocked, and they right-click it to loot it.

## Requirements

- [AzerothCore](https://github.com/azerothcore/azerothcore-wotlk) `master` (WotLK 3.3.5a)
- A WoW 3.3.5a (12340) client

## Install

Clone it into your AzerothCore `modules` folder **as `mod-quick-lockpick`**, without the repo's
`wow-` prefix. AzerothCore finds the module's entry point from the folder name.

```bash
cd <azerothcore>/modules
git clone https://github.com/buildthehomelab/wow-mod-quick-lockpick.git mod-quick-lockpick
```

Rebuild the worldserver, then copy `conf/mod_quick_lockpick.conf.dist` to your config folder as
`mod_quick_lockpick.conf`.

## Settings

| Setting | Default | What it does |
|---------|---------|--------------|
| `QuickLockpick.Enable` | `1` | Master switch. With `0`, lockboxes work as in stock 3.3.5. |
| `QuickLockpick.RightClickPicks` | `1` | Right-clicking a locked box casts Pick Lock on it. |
| `QuickLockpick.OpenAfterUnlock` | `1` | A box in your bags opens as soon as it is unlocked. |

All three can be changed with `.reload config`.

## How it works

The client sends a right-click on a box to the server as "open this item", and the core answers a
locked box with "Item is locked". The module looks at that request first: when the box is locked,
lockpicking can open it and the player knows Pick Lock, it casts Pick Lock on the box instead, the
same cast the player would make by hand.

Once any spell that opens locks finishes on a box in the caster's bags, the module does what a
right-click on an unlocked box does: it sends the loot window. Other modules that hook item
opening still run first.

Playerbots are left alone; they open boxes their own way.
