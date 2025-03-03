# strategy.py - 策略开发框架

import abc
import enum
import time
import datetime
import threading
import queue
import logging
import uuid
import numpy as np
import pandas as pd
from typing import Dict, List, Tuple, Optional, Union, Callable, Any

# 使用PyBind11将C++核心组件导出到Python
import pyquant  # 导入C++核心库的Python绑定

# 订单方向
class OrderSide(enum.Enum):
    BUY = 1
    SELL = 2

# 订单类型
class OrderType(enum.Enum):
    MARKET = 1
    LIMIT = 2
    IOC = 3
    FOK = 4
    STOP = 5
    STOP_LIMIT = 6

# 订单状态
class OrderStatus(enum.Enum):
    NEW = 1
    PARTIALLY_FILLED = 2
    FILLED = 3
    CANCELED = 4
    REJECTED = 5
    EXPIRED = 6

# 时间框架
class Timeframe(enum.Enum):
    TICK = 0
    S1 = 1
    S5 = 5
    M1 = 60
    M5 = 300
    M15 = 900
    M30 = 1800
    H1 = 3600
    H4 = 14400
    D1 = 86400
    W1 = 604800

# 行情数据类
class MarketData:
    def __init__(self, symbol: str):
        self.symbol = symbol
        self.timestamp = 0
        self.last_price = 0.0
        self.last_size = 0
        self.bid_price = 0.0
        self.bid_size = 0
        self.ask_price = 0.0
        self.ask_size = 0
        self.volume = 0
        self.open_interest = 0
        self.open = 0.0
        self.high = 0.0
        self.low = 0.0
        self.close = 0.0
        self.vwap = 0.0
        
        # 高级订单簿数据
        self.bid_levels: List[Tuple[float, int]] = []  # [(价格, 数量), ...]
        self.ask_levels: List[Tuple[float, int]] = []
        self.imbalance = 0.0  # 买卖不平衡比率
        self.spread = 0.0     # 买卖价差
        
    @property
    def mid_price(self) -> float:
        """计算中间价"""
        if self.bid_price > 0 and self.ask_price > 0:
            return (self.bid_price + self.ask_price) / 2
        return self.last_price

# 订单类
class Order:
    def __init__(self, 
                 symbol: str,
                 side: OrderSide,
                 order_type: OrderType,
                 quantity: int,
                 price: float = 0.0,
                 stop_price: float = 0.0):
        self.id = str(uuid.uuid4())
        self.client_order_id = ""
        self.symbol = symbol
        self.side = side
        self.type = order_type
        self.quantity = quantity
        self.price = price
        self.stop_price = stop_price
        self.filled_quantity = 0
        self.average_price = 0.0
        self.status = OrderStatus.NEW
        self.create_time = time.time()
        self.update_time = self.create_time
        self.text = ""  # 错误或信息文本
        
    @property
    def is_active(self) -> bool:
        """检查订单是否仍然活跃"""
        return self.status in [OrderStatus.NEW, OrderStatus.PARTIALLY_FILLED]
        
    @property
    def is_filled(self) -> bool:
        """检查订单是否已完全成交"""
        return self.status == OrderStatus.FILLED
        
    @property
    def is_done(self) -> bool:
        """检查订单是否已完成（成交、取消、拒绝或过期）"""
        return self.status in [OrderStatus.FILLED, OrderStatus.CANCELED, 
                              OrderStatus.REJECTED, OrderStatus.EXPIRED]
                          
    def __str__(self) -> str:
        return (f"Order(id={self.id}, symbol={self.symbol}, "
                f"side={self.side.name}, type={self.type.name}, "
                f"quantity={self.quantity}, filled={self.filled_quantity}, "
                f"price={self.price}, status={self.status.name})")

# 成交类
class Trade:
    def __init__(self, 
                 order_id: str,
                 symbol: str,
                 side: OrderSide,
                 quantity: int,
                 price: float,
                 timestamp: float):
        self.id = str(uuid.uuid4())
        self.order_id = order_id
        self.symbol = symbol
        self.side = side
        self.quantity = quantity
        self.price = price
        self.timestamp = timestamp
        
    def __str__(self) -> str:
        return (f"Trade(id={self.id}, orderId={self.order_id}, "
                f"symbol={self.symbol}, side={self.side.name}, "
                f"quantity={self.quantity}, price={self.price})")

# 仓位类
class Position:
    def __init__(self, symbol: str):
        self.symbol = symbol
        self.quantity = 0
        self.average_price = 0.0
        self.realized_pnl = 0.0
        self.unrealized_pnl = 0.0
        self.open_trades: List[Trade] = []
        
    @property
    def is_flat(self) -> bool:
        """检查仓位是否为平"""
        return self.quantity == 0
        
    @property
    def is_long(self) -> bool:
        """检查是否为多头仓位"""
        return self.quantity > 0
        
    @property
    def is_short(self) -> bool:
        """检查是否为空头仓位"""
        return self.quantity < 0
        
    def update(self, market_data: MarketData) -> None:
        """根据最新市场数据更新未实现盈亏"""
        if self.quantity == 0:
            self.unrealized_pnl = 0.0
            return
            
        current_price = market_data.last_price
        if self.is_long:
            self.unrealized_pnl = (current_price - self.average_price) * self.quantity
        else:
            self.unrealized_pnl = (self.average_price - current_price) * abs(self.quantity)
            
    def __str__(self) -> str:
        return (f"Position(symbol={self.symbol}, quantity={self.quantity}, "
                f"avgPrice={self.average_price}, "
                f"realizedPnL={self.realized_pnl}, unrealizedPnL={self.unrealized_pnl})")

# 账户类
class Account:
    def __init__(self):
        self.id = str(uuid.uuid4())
        self.balance = 0.0
        self.equity = 0.0
        self.margin = 0.0
        self.free_margin = 0.0
        self.positions: Dict[str, Position] = {}
        
    def get_position(self, symbol: str) -> Position:
        """获取指定交易品种的仓位，如不存在则创建"""
        if symbol not in self.positions:
            self.positions[symbol] = Position(symbol)
        return self.positions[symbol]
        
    def update(self, market_data_dict: Dict[str, MarketData]) -> None:
        """更新账户状态"""
        unrealized_pnl = 0.0
        
        # 更新所有仓位
        for symbol, position in self.positions.items():
            if symbol in market_data_dict:
                position.update(market_data_dict[symbol])
                unrealized_pnl += position.unrealized_pnl
                
        # 更新账户权益
        self.equity = self.balance + unrealized_pnl
        self.free_margin = self.equity - self.margin
        
    def __str__(self) -> str:
        positions_str = ", ".join([
            f"{symbol}: {pos.quantity}" for symbol, pos in self.positions.items()
        ])
        return (f"Account(id={self.id}, balance={self.balance}, "
                f"equity={self.equity}, margin={self.margin}, "
                f"positions=[{positions_str}])")

# 策略上下文
class Context:
    def __init__(self):
        self.is_backtest = False
        self.current_datetime = datetime.datetime.now()
        self.account = Account()
        self.symbols: List[str] = []
        self.market_data: Dict[str, MarketData] = {}
        self.orders: Dict[str, Order] = {}
        self.active_orders: Dict[str, Order] = {}
        self.executed_orders: Dict[str, Order] = {}
        self.trades: List[Trade] = []
        self.bars: Dict[str, Dict[Timeframe, pd.DataFrame]] = {}
        
        # 自定义数据存储
        self.data: Dict[str, Any] = {}
        
    def get_market_data(self, symbol: str) -> Optional[MarketData]:
        """获取指定交易品种的市场数据"""
        return self.market_data.get(symbol)
        
    def get_position(self, symbol: str) -> Position:
        """获取指定交易品种的仓位"""
        return self.account.get_position(symbol)
        
    def get_bars(self, symbol: str, timeframe: Timeframe, count: int) -> Optional[pd.DataFrame]:
        """获取历史K线数据"""
        if symbol not in self.bars or timeframe not in self.bars[symbol]:
            return None
            
        df = self.bars[symbol][timeframe]
        if len(df) < count:
            return df
            
        return df.iloc[-count:]

# 策略基类
class Strategy(abc.ABC):
    def __init__(self, name: str = None):
        self.name = name or self.__class__.__name__
        self.context = Context()
        self.logger = logging.getLogger(self.name)
        self.parameters: Dict[str, Any] = {}
        
    def initialize(self) -> None:
        """初始化策略，子类可重写此方法"""
        pass
        
    @abc.abstractmethod
    def on_bar(self, context: Context, bar_dict: Dict[str, pd.DataFrame]) -> None:
        """当收到新K线时调用，必须由子类实现"""
        pass
        
    def on_tick(self, context: Context, market_data: MarketData) -> None:
        """当收到新Tick数据时调用，子类可选择性重写"""
        pass
        
    def on_order_status(self, context: Context, order: Order) -> None:
        """当订单状态变化时调用，子类可选择性重写"""
        pass
        
    def on_trade(self, context: Context, trade: Trade) -> None:
        """当有新成交时调用，子类可选择性重写"""
        pass
        
    def on_position_changed(self, context: Context, position: Position) -> None:
        """当仓位变化时调用，子类可选择性重写"""
        pass
        
    def on_stop(self, context: Context) -> None:
        """当策略停止时调用，子类可选择性重写"""
        pass
        
    def buy(self, symbol: str, quantity: int, price: float = 0.0, 
            order_type: OrderType = OrderType.MARKET) -> Order:
        """发送买单"""
        return self._place_order(symbol, OrderSide.BUY, quantity, price, order_type)
        
    def sell(self, symbol: str, quantity: int, price: float = 0.0, 
             order_type: OrderType = OrderType.MARKET) -> Order:
        """发送卖单"""
        return self._place_order(symbol, OrderSide.SELL, quantity, price, order_type)
        
    def cancel_order(self, order_id: str) -> bool:
        """取消订单"""
        # 在实盘中，这个调用会传递给C++引擎
        # 在回测中，直接修改订单状态
        if self.context.is_backtest:
            if order_id in self.context.active_orders:
                order = self.context.active_orders[order_id]
                order.status = OrderStatus.CANCELED
                order.update_time = time.time()
                del self.context.active_orders[order_id]
                return True
            return False
        else:
            # 调用C++引擎接口
            return pyquant.cancel_order(order_id)
        
    def _place_order(self, symbol: str, side: OrderSide, quantity: int, 
                    price: float, order_type: OrderType) -> Order:
        """内部下单方法"""
        order = Order(symbol, side, order_type, quantity, price)
        
        # 在实盘中，这个调用会传递给C++引擎
        # 在回测中，订单会由回测引擎处理
        if not self.context.is_backtest:
            # 设置客户端订单ID
            order.client_order_id = f"{self.name}_{int(time.time()*1000)}"
            
            # 调用C++引擎接口
            if pyquant.place_order(
                order.symbol, 
                order.side.value, 
                order.type.value, 
                order.quantity, 
                order.price, 
                order.stop_price, 
                order.client_order_id
            ):
                # 添加到活跃订单列表
                self.context.orders[order.id] = order
                self.context.active_orders[order.id] = order
            else:
                # 下单失败
                order.status = OrderStatus.REJECTED
                order.text = "Order placement failed"
        else:
            # 回测模式，直接添加到活跃订单
            self.context.orders[order.id] = order
            self.context.active_orders[order.id] = order
            
        return order

# 事件驱动的策略引擎
class StrategyEngine:
    def __init__(self):
        self.strategies: Dict[str, Strategy] = {}
        self.market_data: Dict[str, MarketData] = {}
        self.bars: Dict[str, Dict[Timeframe, pd.DataFrame]] = {}
        self.event_queue = queue.Queue()
        self.running = False
        self.event_thread = None
        self.logger = logging.getLogger("StrategyEngine")
        
    def add_strategy(self, strategy: Strategy) -> None:
        """添加策略"""
        self.strategies[strategy.name] = strategy
        strategy.context.market_data = self.market_data
        strategy.context.bars = self.bars
        
    def remove_strategy(self, strategy_name: str) -> None:
        """移除策略"""
        if strategy_name in self.strategies:
            del self.strategies[strategy_name]
            
    def initialize(self) -> None:
        """初始化所有策略"""
        for name, strategy in self.strategies.items():
            try:
                strategy.initialize()
                self.logger.info(f"Strategy {name} initialized")
            except Exception as e:
                self.logger.error(f"Error initializing strategy {name}: {e}", exc_info=True)
                
    def start(self) -> None:
        """启动策略引擎"""
        if self.running:
            return
            
        self.running = True
        self.initialize()
        
        # 启动事件处理线程
        self.event_thread = threading.Thread(target=self._event_loop)
        self.event_thread.daemon = True
        self.event_thread.start()
        
        self.logger.info("Strategy engine started")
        
    def stop(self) -> None:
        """停止策略引擎"""
        if not self.running:
            return
            
        self.running = False
        
        # 调用策略的停止回调
        for name, strategy in self.strategies.items():
            try:
                strategy.on_stop(strategy.context)
            except Exception as e:
                self.logger.error(f"Error stopping strategy {name}: {e}", exc_info=True)
                
        # 等待事件线程结束
        if self.event_thread and self.event_thread.is_alive():
            self.event_thread.join(timeout=1.0)
            
        self.logger.info("Strategy engine stopped")
        
    def on_market_data(self, market_data: MarketData) -> None:
        """处理市场数据更新"""
        # 更新内部市场数据缓存
        self.market_data[market_data.symbol] = market_data
        
        # 将事件放入队列
        self.event_queue.put(("tick", market_data))
        
    def on_bar(self, symbol: str, timeframe: Timeframe, bar_data: pd.DataFrame) -> None:
        """处理K线数据更新"""
        # 确保存储结构初始化
        if symbol not in self.bars:
            self.bars[symbol] = {}
        if timeframe not in self.bars[symbol]:
            self.bars[symbol][timeframe] = pd.DataFrame()
            
        # 更新K线数据
        self.bars[symbol][timeframe] = bar_data
        
        # 将事件放入队列
        self.event_queue.put(("bar", (symbol, timeframe)))
        
    def on_order_update(self, order: Order) -> None:
        """处理订单更新"""
        for name, strategy in self.strategies.items():
            # 检查订单是否属于该策略
            for order_id, existing_order in strategy.context.orders.items():
                if order_id == order.id:
                    # 更新订单
                    strategy.context.orders[order_id] = order
                    
                    # 更新活跃订单或执行订单列表
                    if order.is_active:
                        strategy.context.active_orders[order_id] = order
                    else:
                        if order_id in strategy.context.active_orders:
                            del strategy.context.active_orders[order_id]
                        if order.is_filled:
                            strategy.context.executed_orders[order_id] = order
                            
                    # 将事件放入队列
                    self.event_queue.put(("order", (name, order)))
                    break
        
    def on_trade(self, trade: Trade) -> None:
        """处理成交更新"""
        # 将事件放入队列
        for name, strategy in self.strategies.items():
            # 检查成交对应的订单是否属于该策略
            if trade.order_id in strategy.context.orders:
                self.event_queue.put(("trade", (name, trade)))
                
                # 更新策略的成交列表
                strategy.context.trades.append(trade)
        
    def _event_loop(self) -> None:
        """事件处理线程主循环"""
        while self.running:
            try:
                # 从队列获取事件，最多等待0.1秒
                try:
                    event_type, event_data = self.event_queue.get(timeout=0.1)
                except queue.Empty:
                    continue
                    
                # 处理不同类型的事件
                if event_type == "tick":
                    self._process_tick_event(event_data)
                elif event_type == "bar":
                    self._process_bar_event(event_data)
                elif event_type == "order":
                    self._process_order_event(event_data)
                elif event_type == "trade":
                    self._process_trade_event(event_data)
                    
                self.event_queue.task_done()
                
            except Exception as e:
                self.logger.error(f"Error in event loop: {e}", exc_info=True)
                
    def _process_tick_event(self, market_data: MarketData) -> None:
        """处理Tick事件"""
        for name, strategy in self.strategies.items():
            try:
                strategy.on_tick(strategy.context, market_data)
            except Exception as e:
                self.logger.error(f"Error in on_tick for strategy {name}: {e}", exc_info=True)
                
    def _process_bar_event(self, bar_data: Tuple[str, Timeframe]) -> None:
        """处理K线事件"""
        symbol, timeframe = bar_data
        
        # 构建当前所有交易品种的K线字典
        bar_dict = {}
        for name, strategy in self.strategies.items():
            for s in strategy.context.symbols:
                if s in self.bars and timeframe in self.bars[s]:
                    bar_dict[s] = self.bars[s][timeframe]
                    
            try:
                strategy.on_bar(strategy.context, bar_dict)
            except Exception as e:
                self.logger.error(f"Error in on_bar for strategy {name}: {e}", exc_info=True)
                
    def _process_order_event(self, order_data: Tuple[str, Order]) -> None:
        """处理订单事件"""
        strategy_name, order = order_data
        if strategy_name in self.strategies:
            try:
                self.strategies[strategy_name].on_order_status(
                    self.strategies[strategy_name].context, order)
            except Exception as e:
                self.logger.error(
                    f"Error in on_order_status for strategy {strategy_name}: {e}", 
                    exc_info=True)
                    
    def _process_trade_event(self, trade_data: Tuple[str, Trade]) -> None:
        """处理成交事件"""
        strategy_name, trade = trade_data
        if strategy_name in self.strategies:
            strategy = self.strategies[strategy_name]
            
            try:
                # 更新头寸信息
                position = strategy.context.get_position(trade.symbol)
                old_quantity = position.quantity
                
                # 根据成交方向更新头寸
                if trade.side == OrderSide.BUY:
                    position.quantity += trade.quantity
                else:  # SELL
                    position.quantity -= trade.quantity
                    
                # 计算平均价格和已实现盈亏
                # 这里简化了计算逻辑，实际系统需要更复杂的FIFO或LIFO计算
                avg_price = position.average_price
                
                if old_quantity == 0:
                    # 新建仓位
                    position.average_price = trade.price
                elif (old_quantity > 0 and trade.side == OrderSide.BUY) or \
                     (old_quantity < 0 and trade.side == OrderSide.SELL):
                    # 加仓，计算新的平均价格
                    total = abs(old_quantity) * avg_price + trade.quantity * trade.price
                    position.average_price = total / abs(position.quantity)
                elif position.quantity == 0:
                    # 清仓，计算已实现盈亏
                    if trade.side == OrderSide.SELL:
                        position.realized_pnl += (trade.price - avg_price) * trade.quantity
                    else:
                        position.realized_pnl += (avg_price - trade.price) * trade.quantity
                        
                    position.average_price = 0.0
                else:
                    # 减仓但未清仓，部分平仓
                    if (old_quantity > 0 and trade.side == OrderSide.SELL) or \
                       (old_quantity < 0 and trade.side == OrderSide.BUY):
                        if trade.side == OrderSide.SELL:
                            position.realized_pnl += (trade.price - avg_price) * trade.quantity
                        else:
                            position.realized_pnl += (avg_price - trade.price) * trade.quantity
                            
                # 更新账户信息
                strategy.context.account.balance += position.realized_pnl
                
                # 回调通知仓位变化
                strategy.on_position_changed(strategy.context, position)
                
                # 回调通知成交
                strategy.on_trade(strategy.context, trade)
                
            except Exception as e:
                self.logger.error(
                    f"Error processing trade for strategy {strategy_name}: {e}", 
                    exc_info=True)

# 简单移动平均策略示例
class SmaStrategy(Strategy):
    def initialize(self) -> None:
        # 设置策略参数
        self.parameters = {
            "symbol": "AAPL",
            "fast_period": 10,
            "slow_period": 20,
            "trade_size": 100
        }
        
        # 添加要交易的品种
        self.context.symbols = [self.parameters["symbol"]]
        
        self.logger.info(f"SMA策略初始化: {self.parameters}")
        
    def on_bar(self, context: Context, bar_dict: Dict[str, pd.DataFrame]) -> None:
        symbol = self.parameters["symbol"]
        
        if symbol not in bar_dict:
            return
            
        bars = bar_dict[symbol]
        if len(bars) < self.parameters["slow_period"]:
            self.logger.info(f"数据不足: {len(bars)}/{self.parameters['slow_period']}")
            return
            
        # 计算快速和慢速移动平均线
        fast_ma = bars['close'].rolling(self.parameters["fast_period"]).mean()
        slow_ma = bars['close'].rolling(self.parameters["slow_period"]).mean()
        
        # 获取最新值
        current_fast_ma = fast_ma.iloc[-1]
        current_slow_ma = slow_ma.iloc[-1]
        previous_fast_ma = fast_ma.iloc[-2]
        previous_slow_ma = slow_ma.iloc[-2]
        
        # 获取当前仓位
        position = context.get_position(symbol)
        
        # 交易逻辑: 快线上穿慢线买入，快线下穿慢线卖出
        if previous_fast_ma <= previous_slow_ma and current_fast_ma > current_slow_ma:
            # 金叉买入信号
            if position.quantity <= 0:
                # 如果当前无仓位或持有空头，则平仓并买入
                if position.quantity < 0:
                    self.buy(symbol, abs(position.quantity))
                    self.logger.info(f"平空头仓位: {position.quantity}")
                
                # 开多头仓位
                self.buy(symbol, self.parameters["trade_size"])
                self.logger.info(f"金叉买入: {self.parameters['trade_size']}@{bars['close'].iloc[-1]}")
                
        elif previous_fast_ma >= previous_slow_ma and current_fast_ma < current_slow_ma:
            # 死叉卖出信号
            if position.quantity >= 0:
                # 如果当前无仓位或持有多头，则平仓并卖出
                if position.quantity > 0:
                    self.sell(symbol, position.quantity)
                    self.logger.info(f"平多头仓位: {position.quantity}")
                
                # 开空头仓位
                self.sell(symbol, self.parameters["trade_size"])
                self.logger.info(f"死叉卖出: {self.parameters['trade_size']}@{bars['close'].iloc[-1]}")
        
    def on_position_changed(self, context: Context, position: Position) -> None:
        self.logger.info(f"仓位变化: {position}")

# 用于收集策略性能指标的类
class PerformanceTracker:
    def __init__(self, strategy_name: str):
        self.strategy_name = strategy_name
        self.equity_curve = []
        self.trades = []
        self.drawdowns = []
        self.start_time = time.time()
        self.end_time = None
        
    def add_equity_point(self, timestamp: float, equity: float) -> None:
        """添加权益曲线点"""
        self.equity_curve.append((timestamp, equity))
        
    def add_trade(self, trade: Trade) -> None:
        """添加成交记录"""
        self.trades.append(trade)
        
    def calculate_statistics(self) -> Dict[str, Any]:
        """计算性能统计指标"""
        if not self.equity_curve:
            return {}
            
        # 计算最终回报
        initial_equity = self.equity_curve[0][1]
        final_equity = self.equity_curve[-1][1]
        total_return = (final_equity / initial_equity - 1) * 100
        
        # 计算交易次数和盈亏比例
        trade_count = len(self.trades)
        winning_trades = sum(1 for t in self.trades if t.price > 0)  # 简化示例
        win_rate = winning_trades / trade_count if trade_count > 0 else 0
        
        # 计算最大回撤
        max_drawdown = 0
        peak = initial_equity
        
        for _, equity in self.equity_curve:
            if equity > peak:
                peak = equity
            drawdown = (peak - equity) / peak * 100
            max_drawdown = max(max_drawdown, drawdown)
            
        # 计算夏普比率 (简化版)
        returns = []
        for i in range(1, len(self.equity_curve)):
            prev_equity = self.equity_curve[i-1][1]