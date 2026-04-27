#!/usr/bin/env python3
"""
Shared action-label definitions for collection and training.
"""

from __future__ import annotations

DEFAULT_LABEL_MAP = {
    "0": "idle",
    "1": "right_hand_raise",
    "2": "right_hand_slash",
    "3": "walk",
    "4": "run",
    "5": "jump",
    "6": "hands_cross_chest",
    "7": "hands_chest_push",
    "8": "left_hand_raise",
    "9": "both_hands_raise",
}

MULTICLASS_LABEL_ORDER = list(DEFAULT_LABEL_MAP.values())
