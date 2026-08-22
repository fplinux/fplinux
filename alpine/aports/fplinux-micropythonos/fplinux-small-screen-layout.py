# SPDX-License-Identifier: MIT
# mypy: disable-error-code="no-untyped-call, no-untyped-def"
# ruff: noqa: ANN001, ANN201, ANN202, INP001
"""Resolution-derived layouts shared by the FPLinux MicroPythonOS apps."""


def _clamp(value, minimum, maximum):
    return max(minimum, min(value, maximum))


def _padding(width, height):
    return _clamp(min(width, height) // 32, 4, 8)


MAX_RAW_IMAGE_BYTES = 512 * 1024
MAX_IMAGE_FILE_BYTES = 10 * 1024 * 1024


def icon_control_size(width):
    """Return a usable square action-control size for a narrow toolbar."""
    return _clamp(width // 10, 20, 40)


def image_file_size_limit(filename):
    """Return the safe file-size limit for the selected image representation."""
    if filename.lower().endswith(".raw"):
        return MAX_RAW_IMAGE_BYTES
    return MAX_IMAGE_FILE_BYTES


def about_logo_scale(width, padding, source_width=296):
    """Return the LVGL 8.8 fixed-point scale that keeps a logo on screen."""
    available_width = max(1, width - (padding * 2))
    return _clamp((available_width * 256) // source_width, 1, 256)


def connect4_layout(width, height, columns=7, rows=6):
    """Lay out a Connect Four board, status line, and two bottom controls."""
    padding = _padding(width, height)
    gap = padding
    control_height = _clamp(height // 10, 20, 30)
    status_height = _clamp(height // 12, 16, 20)
    board_area_height = height - ((padding * 2) + control_height + status_height + (gap * 2))
    cell_size = max(
        1,
        min(
            (width - (padding * 2)) // columns,
            board_area_height // rows,
        ),
    )
    board_width = columns * cell_size
    board_height = rows * cell_size
    board_x = (width - board_width) // 2
    board_y = padding + ((board_area_height - board_height) // 2)
    button_width = (width - (padding * 3)) // 2

    return {
        "board_pos": (board_x, board_y),
        "board_size": (board_width, board_height),
        "cell_size": cell_size,
        "button_size": (button_width, control_height),
        "difficulty_pos": (padding, height - padding - control_height),
        "new_game_pos": (
            padding * 2 + button_width,
            height - padding - control_height,
        ),
        "status_pos": (padding, padding + board_area_height + gap),
        "status_width": width - (padding * 2),
        "compact": width < 160,
    }


def columns_layout(width, height, columns=6, rows=12):
    """Lay out a Columns board above a two-by-two touch control pad."""
    padding = _padding(width, height)
    gap = padding
    header_height = _clamp(height // 12, 20, 28)
    control_size = _clamp(min(width // 5, height // 10), 20, 32)
    controls_height = (control_size * 2) + gap
    controls_y = height - padding - controls_height
    board_top = padding + header_height
    board_area_height = controls_y - gap - board_top
    cell_size = max(
        1,
        min(
            (width - (padding * 2)) // columns,
            board_area_height // rows,
        ),
    )
    board_width = columns * cell_size
    board_height = rows * cell_size
    board_x = (width - board_width) // 2
    board_y = board_top + ((board_area_height - board_height) // 2)
    controls_x = (width - ((control_size * 2) + gap)) // 2

    return {
        "board_pos": (board_x, board_y),
        "cell_size": cell_size,
        "control_size": (control_size, control_size),
        "left_pos": (controls_x, controls_y),
        "right_pos": (controls_x + control_size + gap, controls_y),
        "rotate_pos": (controls_x, controls_y + control_size + gap),
        "down_pos": (
            controls_x + control_size + gap,
            controls_y + control_size + gap,
        ),
        "score_pos": (padding, padding),
        "compact": width < 160,
    }


def floodit_layout(width, height, columns=10, rows=10, colors=6):
    """Lay out a Flood-It board above a centred two-row color chooser."""
    padding = _padding(width, height)
    gap = _clamp(padding, 4, 6)
    header_height = _clamp(height // 12, 20, 28)
    button_height = _clamp(height // 10, 24, 40)
    button_columns = 3
    button_rows = (colors + button_columns - 1) // button_columns
    controls_height = (button_rows * button_height) + ((button_rows - 1) * gap)
    controls_y = height - padding - controls_height
    board_top = padding + header_height
    board_area_height = controls_y - gap - board_top
    cell_size = max(
        1,
        min(
            (width - (padding * 2)) // columns,
            board_area_height // rows,
        ),
    )
    board_width = columns * cell_size
    board_height = rows * cell_size
    board_x = (width - board_width) // 2
    board_y = board_top + ((board_area_height - board_height) // 2)
    button_width = min(
        56,
        (width - (padding * 2) - ((button_columns - 1) * gap)) // button_columns,
    )
    buttons_width = (button_columns * button_width) + ((button_columns - 1) * gap)
    buttons_x = (width - buttons_width) // 2

    return {
        "board_pos": (board_x, board_y),
        "cell_size": cell_size,
        "button_size": (button_width, button_height),
        "button_pos": lambda index: (
            buttons_x + ((index % button_columns) * (button_width + gap)),
            controls_y + ((index // button_columns) * (button_height + gap)),
        ),
        "score_pos": (padding, padding),
        "compact": width < 160,
    }


def keypad_test_layout(width, height):
    """Fit the keypad test's text field and three status areas on small LCDs."""
    padding = _padding(width, height)
    gap = _clamp(padding, 4, 6)
    title_height = _clamp(height // 16, 14, 20)
    line_height = _clamp(height // 10, 16, 20)
    text_top = padding + title_height + gap
    state_height = line_height * 2
    controls_height = line_height * 3
    max_text_height = height - (padding + text_top + gap + state_height + gap + controls_height)
    text_height = max(1, min(height // 2, max_text_height))
    state_y = text_top + text_height + gap
    controls_y = state_y + state_height + gap

    return {
        "padding": padding,
        "title_height": title_height,
        "text_top": text_top,
        "text_height": text_height,
        "state_pos": (padding, state_y),
        "state_height": state_height,
        "controls_pos": (padding, controls_y),
        "controls_height": controls_height,
        "line_height": line_height,
    }


def imageview_layout(width, height):
    """Keep the file picker and its status label usable on narrow displays."""
    padding = _padding(width, height)
    button_width = _clamp(width // 4, 48, 72)
    button_height = _clamp(height // 8, 24, 40)
    return {
        "button_size": (button_width, button_height),
        "label_width": max(1, width - button_width - (padding * 3)),
        "compact": width < 160,
    }


def game_dashboard_layout(width, height):
    """Reserve separate scoreboard, board, and reset-control areas."""
    padding = _padding(width, height)
    compact = width < 160
    header_height = 20 if compact else 28
    button_height = 24 if compact else 32
    button_width = 48 if compact else 82
    board_y = padding + header_height
    board_height = height - board_y - padding - button_height
    return {
        "padding": padding,
        "compact": compact,
        "board_pos": (padding, board_y),
        "board_size": (width - (padding * 2), max(1, board_height)),
        "reset_size": (button_width, button_height),
        "best_offset": -(button_width // 4) if compact else 0,
    }
