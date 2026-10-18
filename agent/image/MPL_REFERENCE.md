# MPL reference for the delegate

MPL is small, typed and finite. A program is a decision tree over price paths whose nodes are
venue-native orders. It cannot loop except through explicit, counted re-arms. What MPL cannot
express, the venue cannot be asked to run. Units: quantities are integer lots, prices are
integer ticks (1 tick = `tick_cents` cents; for ETH in the examples 1 lot = 1 ETH, 1 tick = $1).

## 1. Grammar (BOUNDED_KEYS.md §6.1)

```
program   := header node+
header    := "instrument" IDENT ("," IDENT)*  "entry" side qty "at" price_expr
node      := IDENT ":" action [ "when" cond ] [ "after" IDENT ] [ "oco" IDENT ("," IDENT)* ] [ "rearm" INT ]
action    := "exit"  qty_expr "at" price_expr "allow" ticks   -- stop-limit with allowance
           | "exit"  qty_expr "market"                        -- stop-market
           | "take"  qty_expr "at" price_expr                 -- limit, reduce-only
           | "scale" side qty "at" price_expr                 -- add to position (limit)
           | "hedge" IDENT side qty "at" price_expr           -- order in another instrument
           | "halt"
cond      := "price" cmp price_expr | "fill" IDENT | "position" cmp qty_expr
cmp       := "<=" | ">="
price_expr:= price | "entry" ("+"|"-") ticks | "entry" ("+"|"-") bps "bps"
qty_expr  := qty | "all" | "half" | "position"
side      := "buy" | "sell"
```

The header is two lines: `instrument ETH` then `entry buy 1 at 3000`. Every other line is a
named node: `name: action ...`. `oco a, b` means the node cancels the named siblings when it
fills. `when price <= X` is the trigger for a stop. `after NODE` makes a node a child of NODE
(it is held until NODE fills); exits and takes without `after` are children of the entry.

## 2. Static rules (§6.2), violations are `400` with a line number

- Every `exit ... at` has `allow >= 1`. An `exit ... at` with no `allow` is rejected before
  verification.
- Exit and take nodes without an explicit `after` are children of the entry: held until it fills.
- Every node is reachable from the entry.
- `rearm` counts are finite; total nodes after expansion <= 64.
- At most 3 instruments.
- Quantities are integer lots and prices integer ticks. No decimals, no fractions, no `bps` unless
  you mean basis points.
- Every program needs a take-profit (`take all at entry + N`, in an `oco` group with the stop).
  Without an upper exit nothing stops a rally, and the key holder's flatten after a rally pays a
  fee on that notional, so the fee bound is unbounded: the verifier answers `422 max_fees_cents`
  with the rally path. After `allow`, a missing take is the most common repair.
- Warning, not error: `allow < env.max_gap_ticks` on a stop-limit. The verifier will almost
  certainly find a gap path on which the stop rests unfilled. Always set `allow` to at least
  `env.max_gap_ticks`.

## 3. The worst loss, and how to size a stop

The verifier models everyone else with the owner's `env`: the market can gap up to
`max_gap_ticks` between prints, fills are full at the print, fees are `fee_bps` (maker on
resting limit fills, taker on stop fills, rounded up to the cent), triggers compare against the
last print.

For a long of `qty` lots with a stop `k` ticks below entry and `allow >= max_gap_ticks`:

```
worst loss (cents) = (k - 1 + max_gap_ticks) * qty * tick_cents + fees
```

The `k - 1` is because the worst trigger print is one tick above the stop level, and the gap
then opens from there. Fees are the entry fee (maker, 1 bp of notional) plus the exit fee (taker,
3.5 bp of notional), each rounded up. The same formula holds for a short with the stop above.

To keep a program inside the envelope choose `k` so that
`(k - 1 + max_gap_ticks) * qty * tick_cents + fees <= max_loss_cents`. If the brief asks for a
stop that does not fit, use the largest `k` that fits, and the smallest `qty` the brief allows.
If `require_live_stop` is true, every program with a position must have an `exit` node.

## 4. Worked example (§11)

Envelope: `max_loss_cents 4000`, `max_gap_ticks 25`, fees maker 1 bp / taker 3.5 bp. Mark 3010.
Brief: buy 1 ETH at 3000, stop 12 ticks below, target 30 ticks above.

### 4.1 First attempt: rejected (`422`)

```mpl
instrument ETH
entry buy 1 at 3000
sl: exit all at entry - 12 allow 5 when price <= entry - 12   oco tp
tp: take all at entry + 30                                    oco sl
```

The stop triggers at 2988 with a limit of 2983 (`allow 5`). The counterexample: the entry fills
at 3000, the last print is 2989, the market gaps 25 ticks to 2964. The stop triggers, but its
limit 2983 is above the print, so it rests unfilled. The next print, 2960, marks the loss at
(3000 - 2960) x $1 + $0.30 entry fee = $40.30 > $40.00. `allow 5` fails because 5 < gap 25: a gap
can jump past the limit and leave the position open.

### 4.2 Second attempt: certified (`200`)

```mpl
instrument ETH
entry buy 1 at 3000
sl: exit all at entry - 12 allow 25 when price <= entry - 12   oco tp
tp: take all at entry + 30                                     oco sl
```

The only change is `allow 25`, which moves the limit to 2963. The worst trigger print is still
2964, and 2964 >= 2963, so the stop fills at 2964. Price loss (3000 - 2964) = $36.00; entry fee
$0.30; exit fee 2964 x 3.5 bp rounded up = $1.04; worst loss $37.34, so `max_loss_cents: 3734`
<= 4000. Check with the formula: (12 - 1 + 25) x 1 x 100 = 3600 cents + 134 fees = 3734. The
highest fees fall on the no-gap path ($1.35). The program makes at most 2 trades.

### 4.3 Third attempt: the turned delegate

If program 4.2 runs, hits its worst path and halts with a realized loss of $37.34, only $2.66 is
left in the window. The identical program certifies at $37.34 but gets `409 budget
{remaining_cents: 266, requested_cents: 3734}`. There is nothing to repair: the budget, not the
program, is what refuses.

## 5. Template

For a brief "buy Q lots at P, stop K ticks below, target T ticks above" with gap G:

```mpl
instrument ETH
entry buy Q at P
sl: exit all at entry - K allow G when price <= entry - K   oco tp
tp: take all at entry + T   oco sl
```

For a short, flip `buy` to `sell`, `entry - K` to `entry + K`, `<=` to `>=`, and `entry + T` to
`entry - T`.

## 6. What the gateway answers

- `200`: a certificate with `bound` (max_loss_cents, max_fees_cents, max_exposure_lots,
  halts_within_trades), the `budget` after reservation and a `bk_` key.
- `422`: a rejection naming `violates` (which envelope limit), `limit`, `value_on_path`, and the
  `counterexample` path. The repair route can fix it for up to 3 rounds.
- `409 budget` or `409 instrument_busy`: the program certified but cannot run now.
- `400`: a static error with `line` and `message`. Fix the line and resubmit.
