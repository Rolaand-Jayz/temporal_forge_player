"""Deterministic mapping between producer arm IDs and canonical campaign
method identities.

The quality campaign produces experiment records whose ``arm_id`` encodes
the runner preset, scale, and CAS placement, for example::

    saved_2.00x_cas20_pre      -> fsr_200x_downsample_resolve_cas20
    saved_2.50x_cas20_post     -> fsr_250x_downsample_external_post_cas20
    saved_3.00x_no_cas         -> fsr_300x_downsample_no_cas
    nativeaa_2.00x_cas20_pre   -> fsr_nativeaa_downsample_resolve_cas20

The review-harness naming contract (``tools/export_review_image.py``)
accepts only the canonical identities on the right-hand side. Producers
call :func:`canonical_method_for_arm` to record the canonical ``method``
next to ``arm_id`` at capture time; consumers may use the same function to
interpret existing records. Arm IDs outside the recognized campaign
vocabulary raise :class:`UnknownArmId` instead of being guessed — an
unrecognized identity must fail clearly rather than be silently
misclassified.
"""

from __future__ import annotations

import re

__all__ = ["UnknownArmId", "canonical_method_for_arm"]


class UnknownArmId(ValueError):
    """Raised when an arm id is not part of the recognized campaign vocabulary."""


# saved/nativeaa preset, fixed two-decimal scale, and either an explicit
# CAS strength with a pre/post placement or the no-CAS arm.
_ARM_RE = re.compile(
    r"^(?P<preset>[a-z0-9]+)_(?P<scale>\d+\.\d{2})x_"
    r"(?:cas(?P<cas>\d+)_(?P<placement>pre|post)|no_cas)$"
)

# Canonical review-harness tiers (fsr_<tier>_downsample_*). "nativeaa"
# overrides the numeric tier derived from the scale.
_KNOWN_TIERS = {"200x", "225x", "250x", "275x", "300x", "nativeaa"}
# Canonical CAS strengths embedded in the method names.
_KNOWN_CAS = {"20"}


def canonical_method_for_arm(arm_id: str) -> str:
    """Return the canonical campaign method id for a producer arm id.

    Raises:
        UnknownArmId: if the arm id is malformed or outside the recognized
            campaign vocabulary. Unknown identities are never guessed.
    """
    match = _ARM_RE.fullmatch(arm_id or "")
    if not match:
        raise UnknownArmId(f"unrecognized FSR4 arm id: {arm_id!r}")
    preset = match.group("preset")
    cas = match.group("cas")
    placement = match.group("placement")
    if preset == "nativeaa":
        tier = "nativeaa"
    else:
        tier = f"{int(round(float(match.group('scale')) * 100)):03d}x"
    if tier not in _KNOWN_TIERS:
        raise UnknownArmId(
            f"arm id {arm_id!r} maps to tier {tier!r}, which is not part of "
            "the canonical campaign method vocabulary"
        )
    if cas is None:
        return f"fsr_{tier}_downsample_no_cas"
    if cas not in _KNOWN_CAS:
        raise UnknownArmId(
            f"arm id {arm_id!r} maps to CAS strength {cas!r}, which is not "
            "part of the canonical campaign method vocabulary"
        )
    stage = "resolve" if placement == "pre" else "external_post"
    return f"fsr_{tier}_downsample_{stage}_cas{cas}"
