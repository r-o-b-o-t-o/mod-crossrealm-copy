DELETE FROM `command` WHERE `name` IN ('character copy');
INSERT INTO `command` (`name`, `security`, `help`) VALUES
('character copy', 0, 'Syntax: .character copy $name\n\nReplaces ALL data of the character you are playing with a copy of the character named $name from the source realm. Requires being logged on a character of the same race and class. You will be disconnected while the copy runs; wait for the confirmation before logging back in.');
