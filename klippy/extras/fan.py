# Printer cooling fan
#
# Copyright (C) 2016-2020  Kevin O'Connor <kevin@koconnor.net>
#
# This file may be distributed under the terms of the GNU GPLv3 license.

# TODO: Is this actually right to just spit it out? 
FAN_MIN_TIME = 0.009

class Fan:
    def __init__(self, config, default_shutdown_speed=0.):
        self.printer = config.get_printer()
        self.last_fan_value = 0.
        self.last_fan_time = 0.
        # Read config
        self.max_power = config.getfloat('max_power', 1., above=0., maxval=1.)
        self.kick_start_time = config.getfloat('kick_start_time', 0.1,
                                               minval=0.)
        self.off_below = config.getfloat('off_below', default=0.,
                                         minval=0., maxval=1.)
        cycle_time = config.getfloat('cycle_time', 0.010, above=0.)
        hardware_pwm = config.getboolean('hardware_pwm', False)
        shutdown_speed = config.getfloat(
            'shutdown_speed', default_shutdown_speed, minval=0., maxval=1.)
        # Setup pwm object
        ppins = self.printer.lookup_object('pins')
        self.mcu_fan = ppins.setup_pin('pwm', config.get('pin'))
        self.mcu_fan.setup_max_duration(0.)
        self.mcu_fan.setup_cycle_time(cycle_time, hardware_pwm)
        shutdown_power = max(0., min(self.max_power, shutdown_speed))
        self.mcu_fan.setup_start_value(0., shutdown_power)
        # Register callbacks
        self.printer.register_event_handler("gcode:request_restart",
                                            self._handle_request_restart)
    def get_mcu(self):
        return self.mcu_fan.get_mcu()
    def set_speed(self, print_time, value):
        if value < self.off_below:
            value = 0.
        value = max(0., min(self.max_power, value * self.max_power))
        if value == self.last_fan_value:
            return
        print_time = max(self.last_fan_time + FAN_MIN_TIME, print_time)
        if (value and value < self.max_power and self.kick_start_time
            and (not self.last_fan_value or value - self.last_fan_value > .5)):
            # Run fan at full speed for specified kick_start_time
            self.mcu_fan.set_pwm(print_time, self.max_power)
            print_time += self.kick_start_time
        self.mcu_fan.set_pwm(print_time, value)
        self.last_fan_time = print_time
        self.last_fan_value = value
    def set_speed_from_command(self, value):
        toolhead = self.printer.lookup_object('toolhead')
        toolhead.register_lookahead_callback((lambda pt:
                                              self.set_speed(pt, value)))
    def _handle_request_restart(self, print_time):
        self.set_speed(print_time, 0.)
    def get_status(self, eventtime):
        return {'speed': self.last_fan_value}

class PrinterFan:
    def __init__(self, config):
        self.fan = Fan(config)
        # Register commands
        gcode = config.get_printer().lookup_object('gcode')
        gcode.register_command("M106", self.cmd_M106)
        gcode.register_command("M107", self.cmd_M107)
    def get_status(self, eventtime):
        return self.fan.get_status(eventtime)
    def cmd_M106(self, gcmd):
        # Set fan speed
        value = gcmd.get_float('S', 255., minval=0.) / 255.
        self.fan.set_speed_from_command(value)
    def cmd_M107(self, gcmd):
        # Turn fan off
        self.fan.set_speed_from_command(0.)

def load_config(config):
    return PrinterFan(config)
