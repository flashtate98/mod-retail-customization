-- Playerbots: the Retail customization choices a new character of each race, sex and class may take, one row
-- per choice. A random bot without stored choices gets one random choice per option at its first login.
-- Filled from the FlashWOW client: run the console command retailBotLooks there and run the
-- Logs\retail_customization_bot_pool.sql it writes on the world database.
CREATE TABLE IF NOT EXISTS `retail_customization_bot_pool` (
  `race` TINYINT UNSIGNED NOT NULL,
  `sex` TINYINT UNSIGNED NOT NULL,
  `class` TINYINT UNSIGNED NOT NULL,
  `option_id` INT UNSIGNED NOT NULL COMMENT 'ChrCustomizationOption ID',
  `choice_id` INT UNSIGNED NOT NULL COMMENT 'ChrCustomizationChoice ID',
  PRIMARY KEY (`race`, `sex`, `class`, `option_id`, `choice_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
