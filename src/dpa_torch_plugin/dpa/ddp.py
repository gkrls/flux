import random
import time
import torch
import torch.nn as nn
import torch.distributed as dist



from threading import Lock

class AtomicCounter:
    def __init__(self, initial=0):
        self._value = initial
        self._lock = Lock()
    
    def add(self, n=1):
        with self._lock:
            val = self._value
            self._value += n
            return val
    
    def inc(self): 
        return self.add(1)
    
    def get(self):
        with self._lock:
            return self._value
    
    def set(self, n=0):
        with self._lock:
            old_val = self._value
            self._value = n  # Fixed: was += n
            return old_val

class DDPStraggleSim:
    """
    Inject delays at:
      - forward_start  (register_forward_pre_hook)
      - forward_end    (register_forward_hook)
      - backward_end   (register_full_backward_hook)

    Parameters
    ----------
    points : int
        Number of hook points (1-3): 1=fwd_start, 2=+fwd_end, 3=+bwd_end
    prob : float
        Probability of applying delay per hook firing (0-100 percent).
    amount : float
        Base delay in seconds (not milliseconds).
    ranks : list[int] or None
        If given, only these ranks will straggle.
    multiplier_range : tuple[float, float]
        Multiplier range applied to 'amount'. Example: (0.5, 2.0).
    skip : int
        Number of steps to skip straggle sim on
    skip_every : int
        Number of consecutive straggle-eligible steps to perform
        before skipping the next `skip` steps. The pattern repeats
        as a cycle of length (skip_every + skip).
    last : int,
        Last step to straggle, after which straggle sim is disabled
    seed : int
        RNG seed for reproducibility.
    verbose : bool
        Print when delays are applied.
    """

    def __init__(self, 
                 points: int = 3, 
                 prob: float = 2.0,
                 amount: float = 2.0,
                 ranks: list[int] | None = None, 
                 multiplier_range: tuple[float, float] = (1.0, 1.0),
                 skip : int = 0,
                 skip_every : int = 0,
                 last = 0,
                 seed: int = None,
                 verbose: bool = False):
        
        if not (0 <= points <= 3): raise ValueError("points must be in [1, 3]")
        self.points = points

        # probability: accept 0..1 or 0..100 (percent)
        if not (0.0 <= prob <= 100.0): raise ValueError("prob must be in [0,100].")
        self.prob = float(prob) / 100 #float(prob / 100 if prob > 1 else prob)

        if amount < 0: raise ValueError("amount must be >= 0.")
        self.amount = float(amount)

        if skip < 0: raise ValueError("skip must be >= 0")
        if skip_every < 0: raise ValueError("skip_frequency must be >= 0")
        if skip == 0 and skip_every > 0: print(f"[straggle_sim][warning] requested skip={skip} but skip_every={skip_every} --> no skips!")
        if skip > 0 and skip_every == 0: 
            print(f"[straggle_sim][warning] requested skip={skip} but skip_every={skip_every} --> skipping only the first {skip} steps")

        self.skip = skip
        self.skip_every = skip_every
        self.skipping = 0

        if last < 0: raise ValueError("last must be >= 0")
        self.last = last

        self.active = self.points > 0 and self.prob > 0 and self.amount > 0
        if not self.active: print(f"[straggle_sim][warning] created but effectively inactive -- points: {self.points}, prob: {self.prob}, amount: {self.amount}")

        self.ranks = set(ranks) if ranks else None

        if multiplier_range is not None:
            a, b = float(multiplier_range[0]), float(multiplier_range[1])
            if not (0 < a <= b): raise ValueError("multiplier_range must satisfy (0 < min_multiplier <= max_multiplier).")
            self.multi_range = (a, b)
        else:
            self.multi_range = (1.0, 1.0)

        # Use different seeds for different RNGs
        base_seed = seed if seed is not None else random.randint(0, 2**32-1)
        self.rng_1 = random.Random(base_seed)
        self.rng_2 = random.Random(base_seed + 42)
        self.verbose = verbose

        self._handles = []
        self._rank = None
        self._step = -1
        self._step_has_straggled = False  # Track if current step already straggled

        self.stats = {
            "num_straggle_steps" : AtomicCounter(),
            "num_straggle_events": AtomicCounter(),
            "total_straggle_time": AtomicCounter()
        }

    def reset_stats(self):
        for counter in self.stats.values():
            counter.set(0)

    def _get_rank_safe(self) -> int:
        """Safely get the current rank."""
        try:
            if dist.is_available() and dist.is_initialized():
                return dist.get_rank()
        except Exception:
            pass
        return 0

    def attach(self, root: nn.Module) -> int:
        if not self.active: return 0

        """Attach hooks to the root model. With DDP, use ddp.module."""
        self.detach()  # idempotent
        self.reset_stats()

        if dist.is_available() and dist.is_initialized():
            self._rank = self._get_rank_safe()
            print(f"[straggle_sim] on rank {self._rank}")
        else:
            self._rank = 0
            if self.verbose: print("[straggle_sim] dist unavailable or not initialized, using rank=0")

        # Filter rank
        if self.ranks is not None and self._rank not in self.ranks:
            print(f"[straggle_sim] rank {self._rank} not in target ranks {self.ranks}, skipping")
            return 0

        # Forward start
        self._handles.append(root.register_forward_pre_hook(self._on_fwd_start))

        # Forward end
        if self.points > 1:
            self._handles.append(root.register_forward_hook(self._on_fwd_end))

        # Backward end
        if self.points > 2:
            try: self._handles.append(root.register_full_backward_hook(self._on_bwd_end))
            except AttributeError: raise ValueError("[straggle_sim] register_full_backward_hook unavailable; use points <= 2")

        print(f"[straggle_sim] attached {len(self._handles)} straggle hooks to rank {self._rank}")

        return len(self._handles)

    def detach(self):
        """Remove all attached hooks."""
        removed = 0
        for h in self._handles:
            try:
                h.remove()
                removed += 1
            except Exception: pass
        self._handles.clear()
        if removed and self.verbose: print(f"[straggle_sim] detached {removed} hook(s).")

    def _sample_delay_seconds(self) -> float:
        """Sample a delay in seconds."""
        if self.multi_range == (1.0, 1.0): return self.amount
        a, b = self.multi_range
        mult = self.rng_2.uniform(a, b)
        return self.amount * mult

    def _done(self) -> bool:
        return self.last > 0 and self._step >= self.last

    def _should_skip(self) -> bool:
        if self.skip <= 0:
            return False

        if self.skip_every == 0:
            return self._step < self.skip

        cycle_len = self.skip_every + self.skip
        pos = self._step % cycle_len

        # Skip the last `skip` steps of the cycle
        return pos >= self.skip_every
        # Skip the first `skip` steps of the cycle
        # return pos < self.skip

    def _maybe_sleep(self, where: str):
        """Maybe inject a delay, tracking stats."""

        if self._done(): return
        if self._should_skip(): return

        if self.rng_1.random() < self.prob:
            delay_sec = self._sample_delay_seconds()
            
            # Update stats
            if not self._step_has_straggled:
                self.stats["num_straggle_steps"].inc()
                self._step_has_straggled = True
            
            self.stats["num_straggle_events"].inc()
            self.stats["total_straggle_time"].add(delay_sec)
            
            if self.verbose:
                print(f"[straggle_sim][rank {self._rank}][step {self._step}] {where}: sleeping {delay_sec:.3f}s "
                      f"(base={self.amount:.3f}s, range={self.multi_range})")
            
            time.sleep(delay_sec)

    # ---- hook callbacks ----
    def _on_fwd_start(self, module, inputs):
        self._step += 1
        self._step_has_straggled = False  # Reset for new step
        self._maybe_sleep("forward_sta")

    def _on_fwd_end(self, module, inputs, outputs):
        self._maybe_sleep("forward_end")

    def _on_bwd_end(self, module, grad_input, grad_output):
        self._maybe_sleep("bckward_end")

    def get_stats(self) -> dict:
        """Get current statistics."""
        return {
            "num_straggle_steps": self.stats["num_straggle_steps"].get(),
            "num_straggle_events": self.stats["num_straggle_events"].get(),
            "total_straggle_time": self.stats["total_straggle_time"].get(),
            "avg_straggle_time": self.stats["total_straggle_time"].get() / max(1, self.stats["num_straggle_events"].get())
        }

    def print_pattern(self):
        msg = f"[straggle_sim] pattern: \n"
        if self.skip <= 0:
            msg += f"  Maybe-straggle at every step (no skipping).\n"
        elif self.skip_every == 0:
            msg += f"  Skip the first {self.skip} step(s), then straggle forever.\n"
        else:
            cycle = self.skip_every + self.skip
            msg += f"  Cycle of {cycle} steps: Maybe-straggle for {self.skip_every}, then clean for {self.skip}.\n"
        msg += (
            f"  At each maybe-straggle step, sleep at {self.points} hook point(s) with probability {self.prob:.1%}.\n"
            f"  Each delay: {self.multi_range[0]:.2f}–{self.multi_range[1]:.2f} × {self.amount:.2f}s."
        )
        if self.last: msg += f"\n  Stops after step {self.last}."

        print(msg)

    # def print_pattern(self):
    #     cycle = self.skip_every + self.skip
    #     msg = (
    #         f"[straggle_sim] pattern: \n"
    #         f"  Alternate between {self.skip} non-straggle step(s) and {self.skip_every} maybe-straggle step(s) (cycle={cycle}).\n"
    #         f"  At each maybe-straggle step, straggle (sleep) at {self.points} points with probability {self.prob:.1%} at each.\n"
    #         f"  Each straggle event lasts between {self.multi_range[0]:.2f}–{self.multi_range[1]:.2f}× {self.amount:.2f}s."
    #     )
    # if self.last:
    #     msg += f" Stops after step {self.last}."

    # print(msg)

    def print_stats(self):
        """Print current statistics."""
        stats = self.get_stats()
        print(f"[straggle_sim] Stats for rank {self._rank}:")
        print(f"  Straggle steps: {stats['num_straggle_steps']}")
        print(f"  Straggle events: {stats['num_straggle_events']}")
        print(f"  Total straggle time: {stats['total_straggle_time']:.1f}s")
        print(f"  Avg straggle time: {stats['avg_straggle_time']:.1f}s")

    def __repr__(self) -> str:
            """Pretty print configuration in one line."""
            ranks_str = f"ranks={list(self.ranks)}" if self.ranks else "all"
            return (f"DDPStraggleSim(points={self.points}, prob={self.prob:.1%}, "
                    f"amount={self.amount:.2f} sec, multiplier_range=[{self.multi_range[0]:.2f},{self.multi_range[1]:.2f}], "
                    f"{ranks_str}, active={self.active})")
    


# from ._ext import DataplaneContext  # Use the new name directly

from . import DataplaneContext, DPADeviceOptions

class _DPAHookState:
  def __init__(self, opts):
    self.opts = opts
    self.world_size = dist.get_world_size()
    self.count = 0


# @torch.no_grad()
# def _dpa_hook(state, bucket):
#     t = bucket.get_tensor() if hasattr(bucket, "get_tensor") else bucket.buffer()
#     idx = bucket.index() if hasattr(bucket, "index") else -1
#     r = dist.get_rank()
#     t0 = time.perf_counter()
#     work = dist.all_reduce(t, op=dist.ReduceOp.AVG, async_op=True)

#     def _done(_):
#         dt = time.perf_counter() - t0
#         print(f"[rank{r}] bucket {idx} complete in {dt:.3f}s")
#         return t

#     return work.get_future().then(_done)

@torch.no_grad()
def _dpa_hook(state, bucket: dist.GradBucket):
    """
    DDP comm hook: must be (state, bucket: GradBucket) and return a Future.
    """

    t = bucket.get_tensor() if hasattr(bucket, "get_tensor") else bucket.buffer()

    # state.opts["quantization"] = torch.is_floating_point(t)
    if (state.opts["prescaled"]): t = t.div_(state.world_size)

    # Run allreduce under our DPA context
    with DataplaneContext(**state.opts, quantization=torch.is_floating_point(t)):  # Use new name
        # Even if we have prescaling, the DPA backend expects an AVG operation
        # Other backends would use SUM here
        # print(f"dataplane allreduce {state.count}")
        state.count += 1;
        work = dist.all_reduce(t, op=torch.distributed.ReduceOp.AVG, async_op=True)
        # print("DPA ALLREDUCE")

  
    # If DPA averages (with or without prescaling), we're done; otherwise divide in-place
    # return work.get_future().then(lambda _: t if (averaging or prescaling) else t.div_(state.world_size))
    # print("returning", t)
    return work.get_future().then(lambda _: t)

  # print("dpa_hook returning")
  # return work.get_future()

def DDPWrapper(ddp_model, *, sa_world: int = 0, sa_preemptive = False, prescale : bool = False, pipes : int = 0, device : DPADeviceOptions = None):
    """
    Patch an existing torch.nn.parallel.DistributedDataParallel with a DPA comm hook.
    """
    opts = dict(
        pipes=pipes if pipes else device["pipes"] if device is not None else 0,
        sa_world=sa_world,
        sa_preemptive=sa_preemptive,
        averaging=True,
        prescaled=prescale,
    )
    state = _DPAHookState(opts)
    ddp_model.register_comm_hook(state, _dpa_hook)
    print("dpa.torch.DDPWrapper:", "_DPAHookState Object: ", opts)
#   if straggle is not None:
#     straggle.register_hooks(ddp_model)

    return ddp_model

def estimate_grad_bucket_max_size(ddp):
    """
    Returns the max bytes a single DDP all-reduce can send.

    - DDP groups gradients into "buckets" up to a size cap (bucket_cap_mb, in MiB).
    - Buckets are formed from whole parameter gradients (assignment is based on param sizes + order).
    - Therefore, the worst-case message size is:
          max(bucket_cap_bytes, size_of_largest_single_param_grad)
    Refs:
      - DDP arg `bucket_cap_mb` (MiB; default 25 MiB):
        https://pytorch.org/docs/stable/generated/torch.nn.parallel.DistributedDataParallel.html
      - How DDP maps parameter gradients into buckets (based on sizes and reverse param order):
        https://pytorch.org/docs/stable/notes/ddp.html
    """
    data = ddp._get_ddp_logging_data()
    cap = data["bucket_bytes_cap"] if isinstance(data, dict) else getattr(data, "bucket_bytes_cap")
    largest = max((p.numel() * p.element_size() for p in ddp.module.parameters() if p.requires_grad), default=0)
    return max(cap, largest)

def estimate_grad_bucket_count(ddp):
    """
    Upper bound on how many DDP all-reduce buckets you'll see per backward.
    DDP never splits a single parameter’s gradient across buckets (i think), so in the worst case
    each grad tensor sits in its own bucket. Thus the bound is just the number of grads.
    """
    return sum(1 for p in ddp.module.parameters() if p.requires_grad)