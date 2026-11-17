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
- Bots show default choices unless rows are added for them.
- Choice IDs are not checked against the DB2s on the server; the client drops any it does not offer.

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
