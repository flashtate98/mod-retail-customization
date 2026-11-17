-- Retail character customization choices that the 3.3.5 appearance fields cannot hold.
-- One row per option a character has set: option_id is a ChrCustomizationOption ID, choice_id the
-- ChrCustomizationChoice ID taken (Retail 12.x DB2 IDs).
CREATE TABLE IF NOT EXISTS `character_retail_customization` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `option_id` INT UNSIGNED NOT NULL,
  `choice_id` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`guid`, `option_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
