-- The Postgres schema (fixed). seed/make-seed.sh creates the tables, bulk-loads the seed, then adds the keys,
-- indexes and foreign keys at the end (marked below), so every index is built compact over the loaded data.
-- The end state is the same as running this file top to bottom on an empty database.
CREATE TABLE users (
  id          BIGSERIAL PRIMARY KEY,
  username    TEXT        NOT NULL,
  created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE posts (
  id          BIGSERIAL PRIMARY KEY,
  user_id     BIGINT      NOT NULL,
  body        TEXT        NOT NULL CHECK (char_length(body) BETWEEN 1 AND 500),
  created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE likes (
  user_id     BIGINT      NOT NULL,
  post_id     BIGINT      NOT NULL,
  created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- after the bulk load --
ALTER TABLE users ADD UNIQUE (username);
ALTER TABLE likes ADD PRIMARY KEY (user_id, post_id);   -- one like per user per post
CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);
CREATE INDEX posts_user_id_idx       ON posts (user_id);
CREATE INDEX likes_post_id_idx       ON likes (post_id);
ALTER TABLE posts ADD FOREIGN KEY (user_id) REFERENCES users(id);
ALTER TABLE likes ADD FOREIGN KEY (user_id) REFERENCES users(id);
ALTER TABLE likes ADD FOREIGN KEY (post_id) REFERENCES posts(id);
SELECT setval('users_id_seq', (SELECT max(id) FROM users));
SELECT setval('posts_id_seq', (SELECT max(id) FROM posts));
