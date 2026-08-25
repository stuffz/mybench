-- Seed for the dev databases in compose.yaml. Applied by `task db:seed`.
--
-- The shape is chosen to exercise the client rather than to model anything:
-- every column type the grid renders, NULLs in editable columns, a table with
-- no primary key (the inspector must refuse to make it editable), foreign keys
-- for the schema graph, a view, and one table big enough that the result grid
-- has to page.

SET NAMES utf8mb4;
SET SESSION cte_max_recursion_depth = 200000;

-- ---------------------------------------------------------------- shop ---
-- The main schema: an ordinary little shop model, FKs included so the schema
-- graph has edges to draw.
CREATE DATABASE IF NOT EXISTS shop CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci;
USE shop;

DROP VIEW IF EXISTS order_totals;
DROP TABLE IF EXISTS order_items, orders, customers, products, categories, audit_log;

CREATE TABLE categories (
    id       INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    name     VARCHAR(64)  NOT NULL,
    parent_id INT UNSIGNED NULL,
    UNIQUE KEY uk_categories_name (name),
    KEY idx_categories_parent (parent_id),
    CONSTRAINT fk_categories_parent FOREIGN KEY (parent_id) REFERENCES categories (id)
        ON DELETE SET NULL ON UPDATE CASCADE
) COMMENT 'Self-referencing so the graph has a self-edge case';

CREATE TABLE products (
    id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    sku         CHAR(13)        NOT NULL,
    name        VARCHAR(160)    NOT NULL,
    category_id INT UNSIGNED    NULL,
    price       DECIMAL(10, 2)  NOT NULL DEFAULT 0.00,
    weight_kg   FLOAT           NULL,
    in_stock    TINYINT(1)      NOT NULL DEFAULT 1,
    status      ENUM('draft','active','discontinued') NOT NULL DEFAULT 'draft',
    tags        JSON            NULL,
    description TEXT            NULL COMMENT 'Long values: exercises column width clamping',
    created_at  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at  TIMESTAMP       NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE KEY uk_products_sku (sku),
    KEY idx_products_category_status (category_id, status),
    CONSTRAINT fk_products_category FOREIGN KEY (category_id) REFERENCES categories (id)
        ON DELETE SET NULL ON UPDATE CASCADE
);

CREATE TABLE customers (
    id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    email      VARCHAR(190)    NOT NULL,
    full_name  VARCHAR(120)    NULL,
    country    CHAR(2)         NULL,
    birthday   DATE            NULL,
    notes      TEXT            NULL,
    created_at DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uk_customers_email (email),
    KEY idx_customers_country (country)
);

CREATE TABLE orders (
    id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    customer_id BIGINT UNSIGNED NOT NULL,
    placed_at   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    shipped_at  DATETIME        NULL,
    total       DECIMAL(12, 2)  NOT NULL DEFAULT 0.00,
    state       ENUM('cart','paid','shipped','refunded') NOT NULL DEFAULT 'cart',
    KEY idx_orders_customer (customer_id),
    KEY idx_orders_placed (placed_at),
    CONSTRAINT fk_orders_customer FOREIGN KEY (customer_id) REFERENCES customers (id)
        ON DELETE CASCADE ON UPDATE CASCADE
);

-- Composite primary key: the grid's edit path has to build a two-column WHERE.
CREATE TABLE order_items (
    order_id   BIGINT UNSIGNED NOT NULL,
    product_id BIGINT UNSIGNED NOT NULL,
    quantity   INT             NOT NULL DEFAULT 1,
    unit_price DECIMAL(10, 2)  NOT NULL,
    PRIMARY KEY (order_id, product_id),
    KEY idx_order_items_product (product_id),
    CONSTRAINT fk_items_order FOREIGN KEY (order_id) REFERENCES orders (id) ON DELETE CASCADE,
    CONSTRAINT fk_items_product FOREIGN KEY (product_id) REFERENCES products (id)
);

-- No primary key on purpose: editability must be refused for this one.
CREATE TABLE audit_log (
    happened_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    actor       VARCHAR(64) NULL,
    action      VARCHAR(64) NOT NULL,
    payload     JSON        NULL,
    KEY idx_audit_happened (happened_at)
) COMMENT 'Heap table, no PK — read-only in the grid';

INSERT INTO categories (name, parent_id) VALUES
    ('Everything', NULL), ('Hardware', 1), ('Peripherals', 2), ('Storage', 2),
    ('Software', 1), ('Licences', 5), ('Clearance', NULL);

INSERT INTO customers (email, full_name, country, birthday, notes) VALUES
    ('ada@example.com',      'Ada Lovelace',    'GB', '1815-12-10', 'First customer'),
    ('grace@example.com',    'Grace Hopper',    'US', '1906-12-09', NULL),
    ('alan@example.com',     'Alan Turing',     'GB', '1912-06-23', NULL),
    ('katherine@example.com','Katherine Johnson','US','1918-08-26', 'Prefers email'),
    ('edsger@example.com',   NULL,              'NL', NULL,         NULL),
    ('barbara@example.com',  'Barbara Liskov',  'US', '1939-11-07', NULL),
    ('no-name@example.com',  NULL,              NULL, NULL,         'NULLs across the row');

INSERT INTO products (sku, name, category_id, price, weight_kg, in_stock, status, tags, description)
VALUES
    ('SKU-000000001', 'Mechanical Keyboard (Blue Switches)', 3, 129.99, 1.2, 1, 'active',
     JSON_ARRAY('input','clicky'), 'A keyboard. The description is deliberately long so the grid has to elide it: it keeps going, and going, past any sensible column width.'),
    ('SKU-000000002', 'Trackball Mouse',        3,  79.50, 0.3, 1, 'active',
     JSON_ARRAY('input'), NULL),
    ('SKU-000000003', 'NVMe SSD 2TB',           4, 189.00, 0.05, 1, 'active',
     JSON_OBJECT('interface','pcie4','tbw',1200), NULL),
    ('SKU-000000004', 'Spinning Rust 8TB',      4, 149.00, 0.7, 0, 'discontinued', NULL, NULL),
    ('SKU-000000005', 'Database Licence (per core)', 6, 2999.00, NULL, 1, 'active',
     JSON_OBJECT('term','annual'), NULL),
    ('SKU-000000006', 'Unpriced Draft Item',    NULL,  0.00, NULL, 1, 'draft', NULL, NULL);

INSERT INTO orders (customer_id, placed_at, shipped_at, total, state) VALUES
    (1, '2026-07-01 09:14:00', '2026-07-02 11:00:00', 209.49, 'shipped'),
    (2, '2026-07-03 14:22:10', NULL,                  189.00, 'paid'),
    (3, '2026-07-11 08:05:42', NULL,                    0.00, 'cart'),
    (4, '2026-08-01 19:30:00', '2026-08-03 07:45:00',2999.00, 'shipped'),
    (1, '2026-08-15 12:00:00', NULL,                   79.50, 'refunded');

INSERT INTO order_items (order_id, product_id, quantity, unit_price) VALUES
    (1, 1, 1, 129.99), (1, 2, 1, 79.50), (2, 3, 1, 189.00),
    (4, 5, 1, 2999.00), (5, 2, 1, 79.50);

INSERT INTO audit_log (actor, action, payload) VALUES
    ('ada',  'login',        JSON_OBJECT('ip','10.0.0.1')),
    ('ada',  'order.create', JSON_OBJECT('order_id',1)),
    (NULL,   'cron.cleanup', NULL),
    ('grace','order.pay',    JSON_OBJECT('order_id',2,'amount',189.00));

CREATE VIEW order_totals AS
SELECT o.id            AS order_id,
       c.email         AS customer_email,
       o.state         AS state,
       COUNT(i.product_id) AS line_count,
       COALESCE(SUM(i.quantity * i.unit_price), 0) AS computed_total
FROM orders o
         JOIN customers c ON c.id = o.customer_id
         LEFT JOIN order_items i ON i.order_id = o.id
GROUP BY o.id, c.email, o.state;

-- --------------------------------------------------------------- bench ---
-- One wide, long table so the result grid has to window, "Fetch All" has
-- something to chew on, and sorting is not instant.
CREATE DATABASE IF NOT EXISTS bench CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci;
USE bench;

DROP TABLE IF EXISTS events;
CREATE TABLE events (
    id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    session_id CHAR(36)        NOT NULL,
    kind       ENUM('view','click','scroll','error') NOT NULL,
    path       VARCHAR(255)    NOT NULL,
    duration_ms INT UNSIGNED   NULL,
    payload    JSON            NULL,
    happened_at DATETIME       NOT NULL,
    KEY idx_events_kind_time (kind, happened_at),
    KEY idx_events_session (session_id)
);

INSERT INTO events (session_id, kind, path, duration_ms, payload, happened_at)
WITH RECURSIVE n (i) AS (
    SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 60000
)
SELECT UUID(),
       ELT(1 + (i % 4), 'view', 'click', 'scroll', 'error'),
       CONCAT('/', ELT(1 + (i % 6), 'home', 'search', 'product', 'cart', 'checkout', 'admin'),
              '/', i % 500),
       -- every 17th row NULL, so the grid renders NULLs in a numeric column
       IF(i % 17 = 0, NULL, (i * 7919) % 5000),
       IF(i % 23 = 0, NULL, JSON_OBJECT('i', i, 'bucket', i % 10)),
       TIMESTAMPADD(SECOND, -(i * 37), '2026-08-22 00:00:00')
FROM n;

-- ------------------------------------------------------------ analytics ---
-- A third schema purely so the sidebar tree and the "hide default databases"
-- preference have more than one user schema to show.
CREATE DATABASE IF NOT EXISTS analytics CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci;
USE analytics;

DROP TABLE IF EXISTS daily_rollup;
CREATE TABLE daily_rollup (
    day        DATE           NOT NULL PRIMARY KEY,
    events     BIGINT UNSIGNED NOT NULL DEFAULT 0,
    errors     BIGINT UNSIGNED NOT NULL DEFAULT 0,
    error_rate DECIMAL(5, 4)  AS (IF(events = 0, 0, errors / events)) STORED,
    note       VARCHAR(255)   NULL
) COMMENT 'Has a generated column — must be read-only in the grid';

INSERT INTO daily_rollup (day, events, errors, note)
WITH RECURSIVE d (i) AS (SELECT 0 UNION ALL SELECT i + 1 FROM d WHERE i < 89)
SELECT DATE_SUB('2026-08-22', INTERVAL i DAY),
       1000 + (i * 137) % 4000,
       (i * 31) % 90,
       IF(i % 10 = 0, CONCAT('checkpoint ', i), NULL)
FROM d;

-- -------------------------------------------------------------- legacy ---
-- A latin1 database with a latin1 table and åäö data: exercises the pinned
-- utf8mb4 session charset — the server must convert at the boundary both on
-- read (grid shows åäö, not � or Ã¥) and on write (editing a cell here must
-- not store mojibake). This file is UTF-8 and the session is SET NAMES
-- utf8mb4, so the literals below are converted to latin1 on insert.
CREATE DATABASE IF NOT EXISTS legacy CHARACTER SET latin1 COLLATE latin1_swedish_ci;
USE legacy;

DROP TABLE IF EXISTS medlemmar;
CREATE TABLE medlemmar (
    id         INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    namn       VARCHAR(80)  NOT NULL,
    ort        VARCHAR(64)  NULL,
    anteckning TEXT         NULL
) CHARACTER SET latin1 COLLATE latin1_swedish_ci
  COMMENT 'latin1 end to end — edits here round-trip through charset conversion';

INSERT INTO medlemmar (namn, ort, anteckning) VALUES
    ('Åsa Öberg',       'Växjö',        'Föredrar leverans på lördagar'),
    ('Örjan Ängström',  'Örnsköldsvik', 'Ändrade adress i våras'),
    ('Märta Sjölund',   'Åmål',         NULL),
    ('Göran Åkesson',   'Härnösand',    'Betalar årsvis — påminn i höst'),
    ('Björn Hägglund',  'Skellefteå',   'Vill ha kvitto per e-post'),
    ('Élise Fältskog',  'Malmö',        'Accents utanför svenskan: é è ü ñ'),
    ('Nils Andersson',  'Ödeshög',      'Ren ASCII i namnet, åäö i orten');
