"""Tkinter view for the board A Modbus service."""

from __future__ import annotations

import tkinter as tk
from tkinter import ttk
from typing import Callable

from .backend import ModbusServiceBackend
from .controller import GuiController
from .model import (
    ALARM_COLORS,
    ALARM_FOREGROUNDS,
    PHASE_CARD_TITLES,
    SENSOR_LAYOUT,
    AlarmLevel,
    ConnectionState,
)
from .ports import list_serial_ports


TEMPERATURE_TREND_COLORS = ("#dc2626", "#15803d", "#2563eb")


class GuiApplication:
    POLL_INTERVAL_MS = 50

    def __init__(
        self,
        root: tk.Tk,
        controller: GuiController,
        *,
        port_lister: Callable[[], list[str]] = list_serial_ports,
    ) -> None:
        self.root = root
        self.controller = controller
        self._port_lister = port_lister
        self._after_id: str | None = None
        self._closing = False
        self._rendered_period = controller.state.period_sec
        self._trend_signature: tuple[int, object, object] | None = None

        self.port_var = tk.StringVar()
        self.address_var = tk.StringVar(value="1")
        self.period_var = tk.StringVar(value="30")
        self.connection_var = tk.StringVar(value="未连接")
        self.identity_var = tk.StringVar(value="设备: --")
        self.error_var = tk.StringVar(value="最近错误: --")
        self.note_var = tk.StringVar(value="")
        self.acquisition_var = tk.StringVar(value="已停止")
        self.completed_var = tk.StringVar(value="0")
        self.failed_var = tk.StringVar(value="0")
        self.last_sample_var = tk.StringVar(value="--")
        self.statistics_vars = {
            "minimum_temperature": tk.StringVar(value="--"),
            "maximum_temperature": tk.StringVar(value="--"),
            "median_temperature": tk.StringVar(value="--"),
            "maximum_delta": tk.StringVar(value="--"),
            "valid_count": tk.StringVar(value="0 / 3"),
            "participating": tk.StringVar(value="参与计算: 无"),
        }
        self.storage_vars = {
            key: tk.StringVar(value="--")
            for key in (
                "storage_state_name",
                "storage_error_name",
                "generated",
                "synced",
                "dropped",
                "event_dropped",
                "uncertain",
                "queued",
                "in_flight",
                "file_identity",
                "last_synced_seq",
            )
        }
        self.card_vars = [
            {
                "temperature": tk.StringVar(value="--"),
                "rom": tk.StringVar(value="--"),
                "quality": tk.StringVar(value="等待采样"),
                "sample_time": tk.StringVar(value="--"),
                "updated_at": tk.StringVar(value="--"),
            }
            for _index in range(3)
        ]
        self.alarm_vars = {
            key: tk.StringVar(value="--")
            for key in (
                "level",
                "reason",
                "trigger_phase",
                "maximum_delta",
                "hottest_temperature",
                "buzzer",
                "acknowledged",
                "event_time",
                "duration",
                "event_id",
                "sample_id",
                "notice_count",
                "warning_count",
                "critical_count",
                "sensor_fault_count",
            )
        }

        self._build_window()
        self._refresh_ports()
        self._render()
        self._after_id = self.root.after(self.POLL_INTERVAL_MS, self._tick)
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def _build_window(self) -> None:
        self.root.title("STM32 板 A DS18B20 温度监控")
        self.root.minsize(1040, 820)
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(6, weight=1)

        style = ttk.Style(self.root)
        if "clam" in style.theme_names():
            style.theme_use("clam")
        style.configure("CardValue.TLabel", font=("TkDefaultFont", 24, "bold"))
        style.configure("Section.TLabel", font=("TkDefaultFont", 11, "bold"))
        style.configure("Error.TLabel", foreground="#9b1c1c")

        self._build_connection_bar()
        self._build_cards()
        self._build_temperature_trend()
        self._build_alarm_panel()
        self._build_statistics()
        self._build_control_area()
        self._build_footer()

    def _build_connection_bar(self) -> None:
        frame = ttk.LabelFrame(self.root, text="连接")
        frame.grid(row=0, column=0, sticky="ew", padx=12, pady=(12, 6))
        frame.columnconfigure(1, weight=1)

        ttk.Label(frame, text="串口").grid(row=0, column=0, padx=(8, 4), pady=6)
        self.port_combo = ttk.Combobox(
            frame,
            textvariable=self.port_var,
            state="readonly",
            width=34,
        )
        self.port_combo.grid(row=0, column=1, sticky="ew", pady=6)
        self.refresh_ports_button = ttk.Button(
            frame,
            text="刷新端口",
            command=self._refresh_ports,
        )
        self.refresh_ports_button.grid(row=0, column=2, padx=4, pady=6)

        ttk.Label(frame, text="Modbus 地址").grid(
            row=0,
            column=3,
            padx=(12, 4),
            pady=6,
        )
        self.address_spinbox = ttk.Spinbox(
            frame,
            from_=1,
            to=247,
            textvariable=self.address_var,
            width=6,
        )
        self.address_spinbox.grid(row=0, column=4, padx=(0, 8), pady=6)

        self.connect_button = ttk.Button(
            frame,
            text="连接",
            command=self._connect,
        )
        self.connect_button.grid(row=0, column=5, padx=4, pady=6)
        self.disconnect_button = ttk.Button(
            frame,
            text="断开",
            command=self.controller.disconnect,
        )
        self.disconnect_button.grid(row=0, column=6, padx=(4, 8), pady=6)

        ttk.Label(frame, text="状态").grid(
            row=1,
            column=0,
            padx=(8, 4),
            pady=(2, 6),
            sticky="w",
        )
        ttk.Label(
            frame,
            textvariable=self.connection_var,
            style="Section.TLabel",
        ).grid(row=1, column=1, sticky="w", pady=(2, 6))
        ttk.Label(frame, textvariable=self.identity_var).grid(
            row=1,
            column=3,
            columnspan=4,
            sticky="w",
            pady=(2, 6),
        )
        ttk.Label(
            frame,
            textvariable=self.error_var,
            style="Error.TLabel",
            wraplength=980,
        ).grid(
            row=2,
            column=0,
            columnspan=7,
            sticky="w",
            padx=8,
            pady=(0, 7),
        )

    def _build_cards(self) -> None:
        frame = ttk.LabelFrame(self.root, text="三路 DS18B20 温度（共用 PG9 / 1-Wire）")
        frame.grid(row=1, column=0, sticky="ew", padx=12, pady=6)
        for column in range(3):
            frame.columnconfigure(column, weight=1, uniform="sensor")

        self.card_status_labels: list[tk.Label] = []
        for column, _sensor_id, location in SENSOR_LAYOUT:
            title = PHASE_CARD_TITLES[column]
            card = ttk.LabelFrame(frame, text=title)
            card.grid(
                row=0,
                column=column,
                sticky="nsew",
                padx=6,
                pady=6,
            )
            card.columnconfigure(1, weight=1)
            status = tk.Label(
                card,
                text=title,
                anchor="w",
                padx=8,
                pady=3,
                background=ALARM_COLORS[AlarmLevel.UNKNOWN],
                foreground=ALARM_FOREGROUNDS[AlarmLevel.UNKNOWN],
            )
            status.grid(
                row=0,
                column=0,
                columnspan=2,
                sticky="ew",
            )
            self.card_status_labels.append(status)
            ttk.Label(card, text=location).grid(
                row=1,
                column=0,
                columnspan=2,
                sticky="w",
                padx=8,
                pady=(6, 4),
            )
            ttk.Label(card, text="温度").grid(
                row=2,
                column=0,
                sticky="w",
                padx=8,
                pady=3,
            )
            ttk.Label(
                card,
                textvariable=self.card_vars[column]["temperature"],
                style="CardValue.TLabel",
            ).grid(row=2, column=1, sticky="e", padx=8, pady=3)
            ttk.Label(card, text="ROM 短标识").grid(
                row=3,
                column=0,
                sticky="w",
                padx=8,
                pady=3,
            )
            ttk.Label(
                card,
                textvariable=self.card_vars[column]["rom"],
            ).grid(row=3, column=1, sticky="e", padx=8, pady=3)
            ttk.Label(card, text="quality").grid(
                row=4,
                column=0,
                sticky="w",
                padx=8,
                pady=3,
            )
            ttk.Label(
                card,
                textvariable=self.card_vars[column]["quality"],
            ).grid(row=4, column=1, sticky="e", padx=8, pady=3)
            ttk.Label(card, text="采样时间(ms)").grid(
                row=5,
                column=0,
                sticky="w",
                padx=8,
                pady=3,
            )
            ttk.Label(
                card,
                textvariable=self.card_vars[column]["sample_time"],
            ).grid(row=5, column=1, sticky="e", padx=8, pady=3)
            ttk.Label(card, text="更新时间").grid(
                row=6,
                column=0,
                sticky="w",
                padx=8,
                pady=(3, 8),
            )
            ttk.Label(
                card,
                textvariable=self.card_vars[column]["updated_at"],
            ).grid(row=6, column=1, sticky="e", padx=8, pady=(3, 8))

    def _build_temperature_trend(self) -> None:
        frame = ttk.LabelFrame(self.root, text="三相温度趋势")
        frame.grid(row=2, column=0, sticky="ew", padx=12, pady=6)
        frame.columnconfigure(0, weight=1)
        self.temperature_trend_canvas = tk.Canvas(
            frame,
            height=150,
            background="#ffffff",
            highlightthickness=0,
        )
        self.temperature_trend_canvas.grid(
            row=0,
            column=0,
            sticky="nsew",
            padx=8,
            pady=(7, 8),
        )
        self.temperature_trend_canvas.bind(
            "<Configure>",
            self._on_temperature_trend_resize,
        )

    def _build_alarm_panel(self) -> None:
        frame = ttk.LabelFrame(self.root, text="三相热告警")
        frame.grid(row=3, column=0, sticky="ew", padx=12, pady=6)
        frame.columnconfigure(1, weight=1)
        frame.columnconfigure(3, weight=1)

        self.alarm_banner = tk.Label(
            frame,
            textvariable=self.alarm_vars["level"],
            anchor="w",
            padx=10,
            pady=7,
            background=ALARM_COLORS[AlarmLevel.UNKNOWN],
            foreground=ALARM_FOREGROUNDS[AlarmLevel.UNKNOWN],
        )
        self.alarm_banner.grid(
            row=0,
            column=0,
            columnspan=4,
            sticky="ew",
            padx=8,
            pady=(7, 5),
        )

        fields = (
            ("原因", "reason"),
            ("触发相", "trigger_phase"),
            ("最大温差", "maximum_delta"),
            ("热点温度", "hottest_temperature"),
            ("蜂鸣器", "buzzer"),
            ("确认状态", "acknowledged"),
            ("事件时间", "event_time"),
            ("持续时间", "duration"),
            ("事件 ID", "event_id"),
            ("alarm sample_id", "sample_id"),
            ("Notice 计数", "notice_count"),
            ("Warning 计数", "warning_count"),
            ("Critical 计数", "critical_count"),
            ("Sensor Fault 计数", "sensor_fault_count"),
        )
        for index, (label, key) in enumerate(fields):
            row = 1 + (index // 2)
            column = (index % 2) * 2
            ttk.Label(frame, text=label).grid(
                row=row,
                column=column,
                sticky="w",
                padx=(8, 4),
                pady=3,
            )
            ttk.Label(
                frame,
                textvariable=self.alarm_vars[key],
            ).grid(
                row=row,
                column=column + 1,
                sticky="w",
                padx=(0, 8),
                pady=3,
            )
        self.ack_button = ttk.Button(
            frame,
            text="确认告警",
            command=self.controller.ack_alarm,
        )
        self.ack_button.grid(
            row=1 + (len(fields) // 2),
            column=0,
            columnspan=4,
            sticky="e",
            padx=8,
            pady=(7, 8),
        )

    def _build_statistics(self) -> None:
        frame = ttk.LabelFrame(self.root, text="有效节点统计")
        frame.grid(row=4, column=0, sticky="ew", padx=12, pady=6)
        fields = (
            ("温度最小", "minimum_temperature"),
            ("温度最大", "maximum_temperature"),
            ("温度中位数", "median_temperature"),
            ("最大温差", "maximum_delta"),
            ("在线节点", "valid_count"),
        )
        for column, (label, key) in enumerate(fields):
            frame.columnconfigure(column, weight=1, uniform="stats")
            ttk.Label(frame, text=label).grid(
                row=0,
                column=column,
                padx=8,
                pady=(7, 1),
            )
            ttk.Label(
                frame,
                textvariable=self.statistics_vars[key],
                style="Section.TLabel",
            ).grid(row=1, column=column, padx=8, pady=(0, 11))
        frame.columnconfigure(len(fields), weight=1)
        ttk.Label(
            frame,
            textvariable=self.statistics_vars["participating"],
        ).grid(
            row=2,
            column=0,
            columnspan=len(fields) + 1,
            sticky="w",
            padx=8,
            pady=(0, 7),
        )

    def _build_control_area(self) -> None:
        container = ttk.Frame(self.root)
        container.grid(row=5, column=0, sticky="nsew", padx=12, pady=6)
        container.columnconfigure(0, weight=1)
        container.columnconfigure(1, weight=1)

        acquisition = ttk.LabelFrame(container, text="采集控制")
        acquisition.grid(row=0, column=0, sticky="nsew", padx=(0, 6))
        acquisition.columnconfigure(5, weight=1)

        ttk.Label(acquisition, text="周期秒数").grid(
            row=0,
            column=0,
            padx=(8, 4),
            pady=7,
        )
        self.period_spinbox = ttk.Spinbox(
            acquisition,
            from_=10,
            to=3600,
            textvariable=self.period_var,
            width=7,
        )
        self.period_spinbox.grid(row=0, column=1, pady=7)
        self.single_button = ttk.Button(
            acquisition,
            text="单次采样",
            command=self.controller.single_sample,
        )
        self.single_button.grid(row=0, column=2, padx=6, pady=7)
        self.start_button = ttk.Button(
            acquisition,
            text="开始周期",
            command=self._start_periodic,
        )
        self.start_button.grid(row=0, column=3, padx=6, pady=7)
        self.stop_button = ttk.Button(
            acquisition,
            text="停止周期",
            command=self.controller.stop_periodic,
        )
        self.stop_button.grid(row=0, column=4, padx=(6, 8), pady=7)

        ttk.Label(acquisition, text="运行状态").grid(
            row=1,
            column=0,
            sticky="w",
            padx=8,
            pady=4,
        )
        ttk.Label(
            acquisition,
            textvariable=self.acquisition_var,
            style="Section.TLabel",
        ).grid(row=1, column=1, columnspan=2, sticky="w", pady=4)
        ttk.Label(acquisition, text="会话完成").grid(
            row=2,
            column=0,
            sticky="w",
            padx=8,
            pady=4,
        )
        ttk.Label(acquisition, textvariable=self.completed_var).grid(
            row=2,
            column=1,
            sticky="w",
            pady=4,
        )
        ttk.Label(acquisition, text="轮询失败").grid(
            row=2,
            column=2,
            sticky="e",
            padx=4,
            pady=4,
        )
        ttk.Label(acquisition, textvariable=self.failed_var).grid(
            row=2,
            column=3,
            sticky="w",
            pady=4,
        )
        ttk.Label(acquisition, text="最近 sample_id").grid(
            row=3,
            column=0,
            sticky="w",
            padx=8,
            pady=(4, 9),
        )
        ttk.Label(acquisition, textvariable=self.last_sample_var).grid(
            row=3,
            column=1,
            columnspan=3,
            sticky="w",
            pady=(4, 9),
        )

        storage = ttk.LabelFrame(container, text="存储状态")
        storage.grid(row=0, column=1, sticky="nsew", padx=(6, 0))
        storage.columnconfigure(1, weight=1)
        storage.columnconfigure(3, weight=1)
        fields = (
            ("状态", "storage_state_name"),
            ("错误", "storage_error_name"),
            ("generated", "generated"),
            ("synced", "synced"),
            ("dropped", "dropped"),
            ("事件丢弃", "event_dropped"),
            ("uncertain", "uncertain"),
            ("queued", "queued"),
            ("in-flight", "in_flight"),
            ("文件身份", "file_identity"),
            ("最后同步序号", "last_synced_seq"),
        )
        for index, (label, key) in enumerate(fields):
            row = index // 2
            column = (index % 2) * 2
            ttk.Label(storage, text=label).grid(
                row=row,
                column=column,
                sticky="w",
                padx=(8, 4),
                pady=3,
            )
            ttk.Label(
                storage,
                textvariable=self.storage_vars[key],
            ).grid(
                row=row,
                column=column + 1,
                sticky="w",
                padx=(0, 8),
                pady=3,
            )
        self.storage_button = ttk.Button(
            storage,
            text="刷新存储状态",
            command=self.controller.refresh_storage,
        )
        self.storage_button.grid(
            row=5,
            column=0,
            columnspan=4,
            sticky="e",
            padx=8,
            pady=(7, 8),
        )

    def _build_footer(self) -> None:
        frame = ttk.Frame(self.root)
        frame.grid(row=6, column=0, sticky="nsew", padx=12, pady=(0, 12))
        frame.columnconfigure(0, weight=1)
        ttk.Label(
            frame,
            textvariable=self.note_var,
            wraplength=980,
        ).grid(row=0, column=0, sticky="nw")
        ttk.Label(
            frame,
            text=(
                "说明：DS18B20 显示的是 PG9 1-Wire 总线温度读数，"
                "显示精度不代表传感器精度。"
                "GUI 的所有设备请求均通过高层 service 在 worker 线程执行。"
            ),
            wraplength=980,
        ).grid(row=1, column=0, sticky="nw", pady=(4, 0))

    def _refresh_ports(self) -> None:
        ports = self._port_lister()
        self.port_combo["values"] = ports
        current = self.port_var.get()
        if current not in ports:
            self.port_var.set(ports[0] if ports else "")

    def _connect(self) -> None:
        try:
            address = int(self.address_var.get())
        except ValueError:
            address = -1
        self.controller.connect(self.port_var.get(), address)

    def _start_periodic(self) -> None:
        self.controller.start_periodic(self.period_var.get())

    def _tick(self) -> None:
        if self._closing:
            return
        self.controller.poll()
        self.controller.tick()
        self._render()
        self._after_id = self.root.after(self.POLL_INTERVAL_MS, self._tick)

    def _on_temperature_trend_resize(self, _event: tk.Event) -> None:
        self._trend_signature = None
        self._draw_temperature_trend()

    def _draw_temperature_trend(self) -> None:
        canvas = self.temperature_trend_canvas
        width = max(canvas.winfo_width(), 1)
        height = max(canvas.winfo_height(), 1)
        history = self.controller.state.temperature_history
        signature = (
            len(history),
            history[0] if history else None,
            history[-1] if history else None,
        )
        if signature == self._trend_signature:
            return
        self._trend_signature = signature
        canvas.delete("all")

        values = [
            value
            for sample in history
            for value in sample
            if value is not None
        ]
        if not values:
            canvas.create_text(
                width / 2,
                height / 2,
                text="等待采样",
                fill="#6b7280",
                tags=("temperature_trend_empty",),
            )
            return

        plot_left = 56
        plot_right = max(width - 56, plot_left + 1)
        plot_top = 18
        plot_bottom = max(height - 24, plot_top + 1)

        legend_x = plot_left
        for phase_name, color in zip(
            ("A 相", "B 相", "C 相"),
            TEMPERATURE_TREND_COLORS,
        ):
            canvas.create_text(
                legend_x,
                6,
                text=phase_name,
                fill=color,
                anchor="nw",
                tags=("temperature_trend_legend",),
            )
            legend_x += 48

        minimum = min(values)
        maximum = max(values)
        if maximum - minimum < 0.01:
            scale_minimum = minimum - 1.0
            scale_maximum = maximum + 1.0
        else:
            padding = max((maximum - minimum) * 0.1, 0.5)
            scale_minimum = minimum - padding
            scale_maximum = maximum + padding

        def y_for(value: float) -> float:
            fraction = (value - scale_minimum) / (
                scale_maximum - scale_minimum
            )
            return plot_bottom - fraction * (plot_bottom - plot_top)

        maximum_y = y_for(maximum)
        minimum_y = y_for(minimum)
        canvas.create_line(
            plot_left,
            maximum_y,
            plot_right,
            maximum_y,
            fill="#9ca3af",
            dash=(4, 3),
            tags=("temperature_trend_reference",),
        )
        canvas.create_line(
            plot_left,
            minimum_y,
            plot_right,
            minimum_y,
            fill="#9ca3af",
            dash=(4, 3),
            tags=("temperature_trend_reference",),
        )
        canvas.create_text(
            plot_right - 4,
            max(plot_top + 2, maximum_y - 9),
            text=f"最高 {maximum:.2f} °C",
            anchor="ne",
            fill="#4b5563",
        )
        canvas.create_text(
            plot_left + 4,
            min(plot_bottom - 2, minimum_y + 9),
            text=f"最低 {minimum:.2f} °C",
            anchor="sw",
            fill="#4b5563",
        )

        sample_count = len(history)

        def x_for(index: int) -> float:
            if sample_count <= 1:
                return (plot_left + plot_right) / 2
            fraction = index / (sample_count - 1)
            return plot_left + fraction * (plot_right - plot_left)

        for phase_index, color in enumerate(TEMPERATURE_TREND_COLORS):
            segment: list[tuple[float, float]] = []
            for sample_index, sample in enumerate(history):
                value = sample[phase_index]
                if value is None:
                    self._draw_temperature_trend_segment(
                        phase_index,
                        color,
                        segment,
                    )
                    segment = []
                    continue
                segment.append((x_for(sample_index), y_for(value)))
            self._draw_temperature_trend_segment(
                phase_index,
                color,
                segment,
            )

    def _draw_temperature_trend_segment(
        self,
        phase_index: int,
        color: str,
        segment: list[tuple[float, float]],
    ) -> None:
        if not segment:
            return
        if len(segment) == 1:
            coordinates = [*segment[0], *segment[0]]
        else:
            coordinates = [
                coordinate
                for point in segment
                for coordinate in point
            ]
        self.temperature_trend_canvas.create_line(
            *coordinates,
            fill=color,
            width=2,
            capstyle=tk.ROUND,
            joinstyle=tk.ROUND,
            tags=(
                "temperature_trend_line",
                f"temperature_trend_phase_{phase_index}",
            ),
        )

    def _render(self) -> None:
        state = self.controller.state
        availability = state.availability()

        self.connection_var.set(state.connection.value)
        identity = state.identity
        if identity is None:
            self.identity_var.set("设备: --")
        else:
            self.identity_var.set(
                "设备: "
                f"{identity.get('reported_version', '--')} / "
                f"protocol {identity.get('protocol_version', '--')}"
            )
        if state.last_error is None:
            self.error_var.set("最近错误: --")
        else:
            timestamp = (
                state.last_error_at.strftime("%Y-%m-%d %H:%M:%S")
                if state.last_error_at is not None
                else "--"
            )
            self.error_var.set(f"最近错误 [{timestamp}]: {state.last_error}")
        self.note_var.set(state.last_note or "")

        for index, card in enumerate(state.sensor_cards()):
            values = self.card_vars[index]
            values["temperature"].set(card.temperature_text)
            values["rom"].set(card.rom_text)
            values["quality"].set(card.quality_text)
            values["sample_time"].set(card.sample_time_text)
            values["updated_at"].set(card.updated_at_text)
        self._draw_temperature_trend()

        stats = state.statistics
        self.statistics_vars["minimum_temperature"].set(
            stats.minimum_temperature_text
        )
        self.statistics_vars["maximum_temperature"].set(
            stats.maximum_temperature_text
        )
        self.statistics_vars["median_temperature"].set(
            stats.median_temperature_text
        )
        self.statistics_vars["maximum_delta"].set(
            stats.maximum_temperature_delta_text
        )
        self.statistics_vars["valid_count"].set(stats.valid_count_text)
        self.statistics_vars["participating"].set(
            stats.participating_sensor_text
        )

        alarm = state.alarm
        if alarm is None:
            alarm_values = {
                "level": "UNKNOWN / 未知",
                "reason": "--",
                "trigger_phase": "--",
                "maximum_delta": "--",
                "hottest_temperature": "--",
                "buzzer": "--",
                "acknowledged": "--",
                "event_time": "--",
                "duration": "--",
                "event_id": "--",
                "sample_id": "--",
                "notice_count": "--",
                "warning_count": "--",
                "critical_count": "--",
                "sensor_fault_count": "--",
            }
            alarm_color = ALARM_COLORS[AlarmLevel.UNKNOWN]
            alarm_foreground = ALARM_FOREGROUNDS[AlarmLevel.UNKNOWN]
        else:
            alarm_values = {
                "level": f"{alarm.level.value} / {alarm.level_label}",
                "reason": alarm.reason_label,
                "trigger_phase": alarm.trigger_phase_label,
                "maximum_delta": alarm.maximum_delta_text,
                "hottest_temperature": alarm.hottest_temperature_text,
                "buzzer": alarm.buzzer_text,
                "acknowledged": alarm.acknowledged_text,
                "event_time": alarm.event_time_text,
                "duration": alarm.duration_text,
                "event_id": str(alarm.event_id),
                "sample_id": str(alarm.alarm_sample_id),
                "notice_count": str(alarm.notice_count),
                "warning_count": str(alarm.warning_count),
                "critical_count": str(alarm.critical_count),
                "sensor_fault_count": str(alarm.sensor_fault_count),
            }
            alarm_color = alarm.color
            alarm_foreground = alarm.foreground
        for key, value in alarm_values.items():
            self.alarm_vars[key].set(value)
        self.alarm_banner.configure(
            background=alarm_color,
            foreground=alarm_foreground,
        )
        for index, label in enumerate(self.card_status_labels):
            label.configure(
                background=alarm_color,
                foreground=alarm_foreground,
                text=(
                    f"{PHASE_CARD_TITLES[index]} / "
                    f"{alarm.level.value if alarm is not None else 'UNKNOWN'}"
                ),
            )

        self.acquisition_var.set(state.acquisition_text)
        self.completed_var.set(str(state.completed_count))
        self.failed_var.set(str(state.failed_count))
        self.last_sample_var.set(
            "--" if state.last_sample_id is None else str(state.last_sample_id)
        )
        if state.period_sec != self._rendered_period:
            self.period_var.set(str(state.period_sec))
            self._rendered_period = state.period_sec

        storage = state.storage
        for key, variable in self.storage_vars.items():
            if key == "file_identity":
                if storage is None:
                    variable.set("--")
                else:
                    file_id = storage.text("last_synced_file")
                    date_id = storage.text("last_synced_date")
                    variable.set(f"{file_id} / {date_id}")
                continue
            variable.set("--" if storage is None else storage.text(key))

        self._set_enabled(self.connect_button, availability.connect)
        self.connect_button.configure(
            text="重连" if state.session_open else "连接"
        )
        self._set_enabled(self.disconnect_button, availability.disconnect)
        self._set_enabled(
            self.refresh_ports_button,
            not state.is_busy
            and state.connection
            not in {ConnectionState.CONNECTING, ConnectionState.CONNECTED},
        )
        port_locked = state.connection in {
            ConnectionState.CONNECTING,
            ConnectionState.CONNECTED,
        }
        self._set_enabled(self.port_combo, not port_locked and not state.is_busy)
        self._set_enabled(self.address_spinbox, not port_locked and not state.is_busy)
        self._set_enabled(
            self.period_spinbox,
            state.connection is ConnectionState.CONNECTED and not state.is_busy,
        )
        self._set_enabled(self.single_button, availability.single_sample)
        self._set_enabled(self.start_button, availability.start_periodic)
        self._set_enabled(self.stop_button, availability.stop_periodic)
        self._set_enabled(self.storage_button, availability.refresh_storage)
        self._set_enabled(self.ack_button, availability.ack_alarm)

    @staticmethod
    def _set_enabled(widget: ttk.Widget, enabled: bool) -> None:
        widget.state(["!disabled"] if enabled else ["disabled"])

    def close(self) -> None:
        if self._closing:
            return
        self._closing = True
        if self._after_id is not None:
            self.root.after_cancel(self._after_id)
            self._after_id = None
        self.controller.shutdown()
        self.root.destroy()


def main(*, demo: bool = False) -> int:
    root = tk.Tk()
    if demo:
        from .demo import DEMO_PORT, DemoBackend

        controller = GuiController(DemoBackend())
        app = GuiApplication(
            root,
            controller,
            port_lister=lambda: [DEMO_PORT],
        )
        app.port_var.set(DEMO_PORT)
        root.after(200, lambda: controller.connect(DEMO_PORT, 1))
    else:
        controller = GuiController(ModbusServiceBackend())
        GuiApplication(root, controller)
    root.mainloop()
    return 0
