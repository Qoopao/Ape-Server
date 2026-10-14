-- Retire only the old group tables. Current im_group_* tables are preserved.
-- Run after deploying code that no longer reads groups/group_members.
-- DDL commits implicitly; this deletes all data in the two old tables.
DROP TABLE IF EXISTS ape_auth.group_members;
DROP TABLE IF EXISTS ape_auth.groups;
