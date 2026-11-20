-- wow-tofuworld-starting-setup: which characters have received the starting kit.
-- Schema: acore_characters. Applied by the worldserver's updater (state MODULE). Re-runnable.
-- Check: SHOW CREATE TABLE tofu_starting_setup;
-- Rows are deleted with the character (OnPlayerDeleteFromDB) and orphans are purged at startup, because the core reuses the GUID of a deleted newest character after a restart.

CREATE TABLE IF NOT EXISTS `tofu_starting_setup` (
    `guid` INT UNSIGNED NOT NULL,
    `granted_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='wow-tofuworld-starting-setup: characters that got the starting kit';
