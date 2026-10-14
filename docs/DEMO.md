# Demo script

A guided tour, about ten minutes. Start the cluster and the dashboard first (see the
root README), then open the dashboard. The panel called **What's happening** narrates
each step as it occurs; **Events** logs it.

Numbers below were measured on real processes; yours will differ a little.

## 1. A healthy cluster

Three nodes, one **leader** (crown), two **followers**, quorum 3 / 3, term 1. Term 1
means no election has happened. Each card shows that node's own **TERM / COMMIT / LOG /
KEYS**. `LOG 1` is the leader's first entry (a no-op it writes on election).

## 2. Write something

In **Key-value console**, press **PUT**, then **GET**. KEYS becomes 1 on every node.
Press **Write 20 keys**: the **Log replication** lanes fill, solid for committed,
faint for entries a node has but that aren't committed yet.

*Idea:* a write is acknowledged only after a majority has it on disk.

## 3. Crash the leader

Press **Kill node** on the leader (a real `kill -9`). Within about a second:
the card goes dashed red, the header says *no leader · election*, a follower becomes a
candidate, wins, and the term goes up. Writes keep working. Press **Revive**: the node
comes back as a *follower*, its lane says *behind N*, then *in sync*.

*Idea:* randomized timeouts, majority votes, and a log that survives a restart.

## 4. Cut the leader off (a partition)

Press **Isolate** on the current leader. It is **not** dead: the dashboard still hears
it directly. Watch:

- the other two elect a new leader in a higher term and keep working;
- the isolated node shows **STALE LEADER**: it still believes it leads the old term;
- try a PUT on the new leader (works) and think about the old one: it answers `503`
  after about two seconds, and so does a linearizable GET. It will not commit, and it
  will not serve possibly stale data;
- the event feed says *lost contact with its peers* and *became leader · term N*.

Press **Reconnect** (or **Heal network**): the stale leader hears the higher term,
**steps down**, receives the majority's writes, and the write it refused was never
applied anywhere.

*Idea:* terms make a stale leader harmless; majorities make it unable to do damage.

## 5. Make the network slow

Drag **Delay each way** to about 40 ms: commits take roughly 80–160 ms, but all is
well. Drag it to 80 ms or more: the panel turns red, leadership starts flapping and
writes fail with `503`. Drag back to 0 and it recovers on its own.

*Idea:* election timeouts must be several times the network round trip.

## 6. Lose the majority

Kill two nodes. Quorum 1 / 3, no leader: PUT and GET both fail with `503`. The cluster
chooses to be unavailable rather than risk answering wrongly. **Revive** one node and
service resumes with nothing lost.

## 7. Durability

Stop the launcher (Ctrl-C) and start it again *without* `--fresh`. Your keys are still
there: each node's log and term were fsynced to disk.

## The same, from a terminal

```bash
curl -L -X PUT --data-binary 'hello' localhost:8102/kv/greeting   # -L follows a follower's 307 to the leader
curl -L localhost:8103/kv/greeting                                # linearizable read
curl "localhost:8103/kv/greeting?stale=1"                         # this node's local copy
curl -N localhost:8101/cluster/events                             # watch the cluster live
curl -X POST localhost:8099/nodes/node-1/kill                     # crash a node
curl -X POST localhost:8099/network/isolate/node-2                # cut a node off
curl -X POST "localhost:8099/network/delay?ms=40"                 # slow the network
curl -X POST localhost:8099/network/reset                         # undo both
```
