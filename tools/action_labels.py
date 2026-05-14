#!/usr/bin/env python3
"""
Shared action-label definitions for collection and training.
"""

from __future__ import annotations

DEFAULT_LABEL_MAP = {
    "0": "idle",
    "1": "right_hand_raise",
    "2": "right_hand_slash",
    "3": "run",
    "4": "walk",
    "5": "hands_cross_forehead",
    "6": "left_hand_raise",
    "7": "ultraman_beam",
    "8": "hands_press_down",
    "9": "kick",
    "a": "jump",
    "b": "turn_body",
    "d": "hands_shoot",
}

MULTICLASS_LABEL_ORDER = list(DEFAULT_LABEL_MAP.values())
