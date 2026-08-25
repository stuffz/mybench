// mybench-seed appends bulk rows to the shop schema of the test server —
// many customers and far more orders — so grids, sorting, paging and the
// dashboard have realistic volume to chew on. It only ever appends: `task
// db:seed` resets the schema and base rows, this piles data on top, and
// running it again just piles on more.
//
// Sizes, DSN and the RNG seed are flags; the RNG is seeded so a given size
// and seed always produce the same data.
package main

import (
	"context"
	"database/sql"
	"errors"
	"flag"
	"fmt"
	"log"
	"math/rand"
	"strings"
	"time"

	_ "github.com/go-sql-driver/mysql"
)

// Rows per INSERT statement: large enough that round-trips do not dominate,
// small enough to stay far under max_allowed_packet.
const batchRows = 2000

const (
	stateShipped  = "shipped"
	stateRefunded = "refunded"
)

func main() {
	dsn := flag.String("dsn", "root:devroot@tcp(127.0.0.1:3307)/shop", "MySQL DSN of the test server")
	customers := flag.Int("customers", 100, "customers to append")
	orders := flag.Int("orders", 1_000_000, "orders to append")
	seed := flag.Int64("seed", 1, "RNG seed — same seed and sizes give the same data")
	flag.Parse()

	if err := run(*dsn, *customers, *orders, *seed); err != nil {
		log.Fatal(err)
	}
}

func run(dsn string, customers, orders int, seed int64) error {
	// Deterministic test data is the point — same seed, same rows.
	rng := rand.New(rand.NewSource(seed)) //nolint:gosec // not security-sensitive: reproducible seed data

	db, err := sql.Open("mysql", dsn)
	if err != nil {
		return fmt.Errorf("open: %w", err)
	}
	defer func() { _ = db.Close() }()
	ctx := context.Background()
	if perr := db.PingContext(ctx); perr != nil {
		return fmt.Errorf("connect %s: %w", dsn, perr)
	}

	start := time.Now()
	if cerr := seedCustomers(ctx, db, rng, customers); cerr != nil {
		return fmt.Errorf("customers: %w", cerr)
	}
	ids, err := customerIDs(ctx, db)
	if err != nil {
		return fmt.Errorf("customer ids: %w", err)
	}
	if oerr := seedOrders(ctx, db, rng, ids, orders); oerr != nil {
		return fmt.Errorf("orders: %w", oerr)
	}
	log.Printf("done: +%d customers, +%d orders in %s", customers, orders, time.Since(start).Round(time.Second))
	return nil
}

// insertBatched runs `INSERT INTO … VALUES` statements of up to batchRows
// rows each; fill appends one row's values to args.
func insertBatched(ctx context.Context, db *sql.DB, insert, row string, n int, fill func(i int, args []any) []any) error {
	for done := 0; done < n; {
		k := min(batchRows, n-done)
		// Placeholder groups only; every value rides in args.
		query := insert + row + strings.Repeat(","+row, k-1) //nolint:gosec // no user data in the SQL text
		args := make([]any, 0, k*strings.Count(row, "?"))
		for i := range k {
			args = fill(done+i, args)
		}
		if _, err := db.ExecContext(ctx, query, args...); err != nil {
			return fmt.Errorf("insert batch: %w", err)
		}
		done += k
		if done%200000 == 0 && done < n {
			log.Printf("  %d/%d rows", done, n)
		}
	}
	return nil
}

func seedCustomers(ctx context.Context, db *sql.DB, rng *rand.Rand, n int) error {
	if n <= 0 {
		return nil
	}
	// Emails continue from the current MAX(id), so appending twice never
	// trips the unique key.
	var base int64
	if err := db.QueryRowContext(ctx, "SELECT COALESCE(MAX(id), 0) FROM customers").Scan(&base); err != nil {
		return fmt.Errorf("max customer id: %w", err)
	}

	first := []string{
		"Ada", "Alan", "Barbara", "Edsger", "Grace", "Katherine", "Donald", "Leslie",
		"Margaret", "Dennis", "Ken", "Radia", "Frances", "John", "Niklaus", "Bjarne",
	}
	last := []string{
		"Lovelace", "Turing", "Liskov", "Dijkstra", "Hopper", "Johnson", "Knuth", "Lamport",
		"Hamilton", "Ritchie", "Thompson", "Perlman", "Allen", "Backus", "Wirth", "Stroustrup",
	}
	countries := []string{"SE", "US", "GB", "DE", "NL", "FR", "NO", "DK", "FI", "JP"}

	return insertBatched(ctx, db,
		"INSERT INTO customers (email, full_name, country, birthday, notes) VALUES ",
		"(?,?,?,?,?)", n,
		func(i int, args []any) []any {
			email := fmt.Sprintf("seed-%d@example.com", base+int64(i)+1)
			var name, country, birthday, notes any
			if rng.Intn(20) != 0 { // the odd NULL name, like the base rows
				name = first[rng.Intn(len(first))] + " " + last[rng.Intn(len(last))]
			}
			if rng.Intn(10) != 0 {
				country = countries[rng.Intn(len(countries))]
			}
			if rng.Intn(4) != 0 {
				birthday = fmt.Sprintf("%d-%02d-%02d", 1950+rng.Intn(56), 1+rng.Intn(12), 1+rng.Intn(28))
			}
			if rng.Intn(10) == 0 {
				notes = "bulk-seeded"
			}
			return append(args, email, name, country, birthday, notes)
		})
}

func customerIDs(ctx context.Context, db *sql.DB) ([]int64, error) {
	rows, err := db.QueryContext(ctx, "SELECT id FROM customers")
	if err != nil {
		return nil, fmt.Errorf("select customers: %w", err)
	}
	defer func() { _ = rows.Close() }()
	var ids []int64
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			return nil, fmt.Errorf("scan customer id: %w", err)
		}
		ids = append(ids, id)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("read customers: %w", err)
	}
	return ids, nil
}

func seedOrders(ctx context.Context, db *sql.DB, rng *rand.Rand, customerIDs []int64, n int) error {
	if n <= 0 {
		return nil
	}
	if len(customerIDs) == 0 {
		return errors.New("no customers to attach orders to")
	}
	// A fixed anchor rather than time.Now(), so the data is reproducible.
	anchor := time.Date(2026, 8, 22, 0, 0, 0, 0, time.UTC)

	return insertBatched(ctx, db,
		"INSERT INTO orders (customer_id, placed_at, shipped_at, total, state) VALUES ",
		"(?,?,?,?,?)", n,
		func(i int, args []any) []any {
			customer := customerIDs[rng.Intn(len(customerIDs))]
			placed := anchor.Add(-time.Duration(rng.Intn(730*24*3600)) * time.Second)
			total := float64(rng.Intn(299500)+500) / 100
			// Mostly shipped, a few carts and refunds — roughly a real shop.
			var state string
			var shipped any
			switch r := rng.Intn(100); {
			case r < 10:
				state = "cart"
				total = 0
			case r < 40:
				state = "paid"
			case r < 95:
				state = stateShipped
			default:
				state = stateRefunded
			}
			if state == stateShipped || state == stateRefunded {
				shipped = placed.Add(time.Duration(1+rng.Intn(72)) * time.Hour).Format(time.DateTime)
			}
			return append(args, customer, placed.Format(time.DateTime), shipped, total, state)
		})
}
