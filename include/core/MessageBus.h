# strategy.py - Strategy Development Framework

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

# Use PyBind11 to export C++ core components to Python
import pyquant  # Import Python bindings for the C++ core library

# Order Side
class OrderSide(enum.Enum):
    BUY = 1
    SELL = 2

# Order Type
class OrderType(enum.Enum):
    MARKET = 1
    LIMIT = 2
    IOC = 3
    FOK = 4
    STOP = 5
    STOP_LIMIT = 6

# Order Status
class OrderStatus(enum.Enum):
    NEW = 1
    PARTIALLY_FILLED = 2
    FILLED = 3
    CANCELED = 4
    REJECTED = 5
    EXPIRED = 6

# Timeframe
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

# Market Data Class
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
        
        # Advanced order book data
        self.bid_levels: List[Tuple[float, int]] = []  # [(price, quantity), ...]
        self.ask_levels: List[Tuple[float, int]] = []
        self.imbalance = 0.0  # Buy/sell imbalance ratio
        self.spread = 0.0     # Bid/ask spread
        
    @property
    def mid_price(self) -> float:
        """Calculate the mid-price"""
        if self.bid_price > 0 and self.ask_price > 0:
            return (self.bid_price + self.ask_price) / 2
        return self.last_price

# Order Class
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
        self.text = ""  # Error or informational text
        
    @property
    def is_active(self) -> bool:
        """Check if the order is still active"""
        return self.status in [OrderStatus.NEW, OrderStatus.PARTIALLY_FILLED]
        
    @property
    def is_filled(self) -> bool:
        """Check if the order is fully filled"""
        return self.status == OrderStatus.FILLED
        
    @property
    def is_done(self) -> bool:
        """Check if the order is done (filled, canceled, rejected, or expired)"""
        return self.status in [OrderStatus.FILLED, OrderStatus.CANCELED, 
                              OrderStatus.REJECTED, OrderStatus.EXPIRED]
                          
    def __str__(self) -> str:
        return (f"Order(id={self.id}, symbol={self.symbol}, "
                f"side={self.side.name}, type={self.type.name}, "
                f"quantity={self.quantity}, filled={self.filled_quantity}, "
                f"price={self.price}, status={self.status.name})")

# Trade Class
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

# Position Class
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
        """Check if the position is flat"""
        return self.quantity == 0
        
    @property
    def is_long(self) -> bool:
        """Check if it's a long position"""
        return self.quantity > 0
        
    @property
    def is_short(self) -> bool:
        """Check if it's a short position"""
        return self.quantity < 0
        
    def update(self, market_data: MarketData) -> None:
        """Update unrealized P&L based on the latest market data"""
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

# Account Class
class Account:
    def __init__(self):
        self.id = str(uuid.uuid4())
        self.balance = 0.0
        self.equity = 0.0
        self.margin = 0.0
        self.free_margin = 0.0
        self.positions: Dict[str, Position] = {}
        
    def get_position(self, symbol: str) -> Position:
        """Get the position for the specified symbol, create it if it doesn't exist"""
        if symbol not in self.positions:
            self.positions[symbol] = Position(symbol)
        return self.positions[symbol]
        
    def update(self, market_data_dict: Dict[str, MarketData]) -> None:
        """Update account status"""
        unrealized_pnl = 0.0
        
        # Update all positions
        for symbol, position in self.positions.items():
            if symbol in market_data_dict:
                position.update(market_data_dict[symbol])
                unrealized_pnl += position.unrealized_pnl
                
        # Update account equity
        self.equity = self.balance + unrealized_pnl
        self.free_margin = self.equity - self.margin
        
    def __str__(self) -> str:
        positions_str = ", ".join([
            f"{symbol}: {pos.quantity}" for symbol, pos in self.positions.items()
        ])
        return (f"Account(id={self.id}, balance={self.balance}, "
                f"equity={self.equity}, margin={self.margin}, "
                f"positions=[{positions_str}])")

# Strategy Context
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
        
        # Custom data storage
        self.data: Dict[str, Any] = {}
        
    def get_market_data(self, symbol: str) -> Optional[MarketData]:
        """Get the market data for the specified symbol"""
        return self.market_data.get(symbol)
        
    def get_position(self, symbol: str) -> Position:
        """Get the position for the specified symbol"""
        return self.account.get_position(symbol)
        
    def get_bars(self, symbol: str, timeframe: Timeframe, count: int) -> Optional[pd.DataFrame]:
        """Get historical candlestick data"""
        if symbol not in self.bars or timeframe not in self.bars[symbol]:
            return None
            
        df = self.bars[symbol][timeframe]
        if len(df) < count:
            return df
            
        return df.iloc[-count:]

# Strategy Base Class
class Strategy(abc.ABC):
    def __init__(self, name: str = None):
        self.name = name or self.__class__.__name__
        self.context = Context()
        self.logger = logging.getLogger(self.name)
        self.parameters: Dict[str, Any] = {}
        
    def initialize(self) -> None:
        """Initialize the strategy, subclasses can override this method"""
        pass
        
    @abc.abstractmethod
    def on_bar(self, context: Context, bar_dict: Dict[str, pd.DataFrame]) -> None:
        """Called when a new candlestick is received, must be implemented by subclasses"""
        pass
        
    def on_tick(self, context: Context, market_data: MarketData) -> None:
        """Called when new tick data is received, subclasses can optionally override"""
        pass
        
    def on_order_status(self, context: Context, order: Order) -> None:
        """Called when an order's status changes, subclasses can optionally override"""
        pass
        
    def on_trade(self, context: Context, trade: Trade) -> None:
        """Called when a new trade occurs, subclasses can optionally override"""
        pass
        
    def on_position_changed(self, context: Context, position: Position) -> None:
        """Called when a position changes, subclasses can optionally override"""
        pass
        
    def on_stop(self, context: Context) -> None:
        """Called when the strategy stops, subclasses can optionally override"""
        pass
        
    def buy(self, symbol: str, quantity: int, price: float = 0.0, 
            order_type: OrderType = OrderType.MARKET) -> Order:
        """Send a buy order"""
        return self._place_order(symbol, OrderSide.BUY, quantity, price, order_type)
        
    def sell(self, symbol: str, quantity: int, price: float = 0.0, 
             order_type: OrderType = OrderType.MARKET) -> Order:
        """Send a sell order"""
        return self._place_order(symbol, OrderSide.SELL, quantity, price, order_type)
        
    def cancel_order(self, order_id: str) -> bool:
        """Cancel an order"""
        # In live trading, this call will be passed to the C++ engine
        # In backtesting, directly modify the order status
        if self.context.is_backtest:
            if order_id in self.context.active_orders:
                order = self.context.active_orders[order_id]
                order.status = OrderStatus.CANCELED
                order.update_time = time.time()
                del self.context.active_orders[order_id]
                return True
            return False
        else:
            # Call the C++ engine interface
            return pyquant.cancel_order(order_id)
        
    def _place_order(self, symbol: str, side: OrderSide, quantity: int, 
                    price: float, order_type: OrderType) -> Order:
        """Internal order placement method"""
        order = Order(symbol, side, order_type, quantity, price)
        
        # In live trading, this call will be passed to the C++ engine
        # In backtesting, orders will be processed by the backtesting engine
        if not self.context.is_backtest:
            # Set the client order ID
            order.client_order_id = f"{self.name}_{int(time.time()*1000)}"
            
            # Call the C++ engine interface
            if pyquant.place_order(
                order.symbol, 
                order.side.value, 
                order.type.value, 
                order.quantity, 
                order.price, 
                order.stop_price, 
                order.client_order_id
            ):
                # Add to the active orders list
                self.context.orders[order.id] = order
                self.context.active_orders[order.id] = order
            else:
                # Order placement failed
                order.status = OrderStatus.REJECTED
                order.text = "Order placement failed"
        else:
            # Backtesting mode, directly add to active orders
            self.context.orders[order.id] = order
            self.context.active_orders[order.id] = order
            
        return order

# Event-Driven Strategy Engine
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
        """Add a strategy"""
        self.strategies[strategy.name] = strategy
        strategy.context.market_data = self.market_data
        strategy.context.bars = self.bars
        
    def remove_strategy(self, strategy_name: str) -> None:
        """Remove a strategy"""
        if strategy_name in self.strategies:
            del self.strategies[strategy_name]
            
    def initialize(self) -> None:
        """Initialize all strategies"""
        for name, strategy in self.strategies.items():
            try:
                strategy.initialize()
                self.logger.info(f"Strategy {name} initialized")
            except Exception as e:
                self.logger.error(f"Error initializing strategy {name}: {e}", exc_info=True)
                
    def start(self) -> None:
        """Start the strategy engine"""
        if self.running:
            return
            
        self.running = True
        self.initialize()
        
        # Start the event processing thread
        self.event_thread = threading.Thread(target=self._event_loop)
        self.event_thread.daemon = True
        self.event_thread.start()
        
        self.logger.info("Strategy engine started")
        
    def stop(self) -> None:
        """Stop the strategy engine"""
        if not self.running:
            return
            
        self.running = False
        
        # Call the strategy's stop callback
        for name, strategy in self.strategies.items():
            try:
                strategy.on_stop(strategy.context)
            except Exception
