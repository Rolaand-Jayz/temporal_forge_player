# Swarm agent orchestrator

`matrix.json` is the complete, explicit configuration matrix. Planning expands
its axes and writes a resumable job manifest; planning does not start a player.

```bash
python3 benchmarks/quality_sweeps/swarm/agent_orchestrator/sweep_orchestrator.py \
  plan --manifest /tmp/tforge-swarm/jobs.json
python3 benchmarks/quality_sweeps/swarm/agent_orchestrator/sweep_orchestrator.py \
  run --manifest /tmp/tforge-swarm/jobs.json \
  --binary ./build/temporal_forge_player \
  --output-root /tmp/tforge-swarm/results
```

The second command is still capture-free unless `--execute-captures` is
provided. With that explicit flag, jobs are serialized, free space is checked
before every attempt, failed jobs are retried without overwriting attempts,
and each job delegates to `run_quality_sweep.py --workers 1`. The child runner
owns capture and metric production; this directory only owns planning,
provenance, status, and resume policy. The job manifest is atomically rewritten
after each attempt.

The default output contract is CSV-and-provenance-only at the orchestrator
boundary. Existing capture runners may retain their own required review assets
when execution is explicitly enabled. Exact `output_width` and `output_height`
values are checked against the matrix after a successful child run.

Capture-free self-test:

```bash
cd benchmarks/quality_sweeps/swarm/agent_orchestrator
python3 selftest.py
```
