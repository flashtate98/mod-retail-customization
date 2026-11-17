# mod-retail-customization

Saves the Retail character customization choices (Eyebrows, Ears, Eye Color, Necklace, ...) that a
FlashWOW client picks on the create screen, and sends them to every client that shows the character,
so other players see the same look. The five 3.3.5 appearance fields (skin, face, hair style, hair
colour, facial hair) still go through the normal character data.

## Install

1. Copy this folder into `azerothcore-wotlk/modules/` (folder name `mod-retail-customization`).
2. Re-run CMake and rebuild the worldserver.
3. The table is created from `data/sql/db-characters/base/character_retail_customization.sql` by the
   DB updater on the next start. If module SQL updates are off, run that file on the characters DB.
4. Copy `conf/mod_retail_customization.conf.dist` to your config folder as
   `mod_retail_customization.conf` (optional; it defaults to enabled).

The worldserver logs `>> Retail customization: N choices of M characters loaded` at startup.

## Protocol

See the comment at the top of `src/RetailCustomization.cpp`. Client side: patch 0024
(`WoW/Source/RetailCustomizationNet.cpp`).

## Notes

- Characters made before the module (or by stock clients) have no rows and show default choices.
- Random playerbots get a look at their first login (see Playerbots below); other bots show default choices.
- Choice IDs are not checked against the DB2s on the server; the client drops any it does not offer.

## Playerbots

A random bot (an account whose name starts with `RetailCustomization.BotAccountPrefix`, `RNDBOT` by default)
that has no stored choices is given one random choice per option of its race, sex and class at its first
login. The choices are saved like a player's, so the bot keeps that look, and everyone nearby is sent it.
Bots that already have choices are left alone; altbots (players' own characters) are never touched.

The choices come from `retail_customization_bot_pool` in the world database
(`data/sql/db-world/base/retail_customization_bot_pool.sql`). Fill it from the FlashWOW client: open the
console, run `retailBotLooks`, and run the `Logs\retail_customization_bot_pool.sql` it writes on the world
database. Run it again after changing the customization DB2s or the race map. The worldserver logs
`>> Retail customization: N bot choices for M race, sex and class combinations loaded` at startup.

A Worgen or Dracthyr bot also gets a random look for its other form (Gilnean, Visage), from the pool rows of
that form's race (54, 55), and starts in either form (`TwoForms.BotAlteredChance`). A Worgen bot that a fight
turns back into a wolf returns to its Gilnean form afterwards (`TwoForms.BotReturnAfterCombat`). The pool
carries the values each 3.3.5 selector may take too (options 0xFFFFFF10-0xFFFFFF14), for that other form.

## Two Forms (FlashWOW)

A Worgen keeps two looks: the wolf and the Gilnean (human) form, each with its own five appearance
values and Retail choices. The active one is stored as usual; the other is in `character_two_forms`
and `character_two_forms_choices` (`data/sql/db-characters/base/character_two_forms.sql`).

- Casting `TwoForms.ToggleSpell` swaps the two looks (not in combat) and sets or clears
  `UNIT_FLAG2_ALTERED_FORM`; clients then draw the Gilnean race (54). The form is kept across logout.
- `TwoForms.NativeFormSpells` send a player in Gilnean form back to the wolf (Darkflight, Running Wild).
- The barber shop edits whichever form is showing.
- A new Worgen starts in wolf form; the Gilnean form starts at its first look (all values 0) until it
  is changed at the barber shop.
- Needs the core patch `acore_two_forms.patch` (the flag, `AlteredFormRace` and the barber shop race
  check) and the Gilnean BarberShopStyle rows in the server's DBC folder.
