import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = []
DEPENDENCIES = ["spi"]
MULTI_CONF = False

liw01_counter_v030_ns = cg.esphome_ns.namespace("liw01_counter_v030")
LIW01Counter = liw01_counter_v030_ns.class_("LIW01Counter", cg.PollingComponent)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(LIW01Counter),
    }
).extend(cv.polling_component_schema("1s"))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
