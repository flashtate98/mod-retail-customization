-- FlashWOW Two Forms: the form a character of a race with an altered form (Worgen) shows, and the look of
-- its other form. The active form's look is in the normal places (characters, character_retail_customization).
CREATE TABLE IF NOT EXISTS `character_two_forms` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `form` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 the race''s own form is active, 1 its altered form',
  `skin` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'the other form''s appearance',
  `face` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `hair_style` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `hair_color` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `facial_hair` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- The other form's Retail choices (same layout as character_retail_customization).
CREATE TABLE IF NOT EXISTS `character_two_forms_choices` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `option_id` INT UNSIGNED NOT NULL,
  `choice_id` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`guid`, `option_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
