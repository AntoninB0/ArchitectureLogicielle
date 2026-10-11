#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

typedef enum {
    APPUI = 1,
} event_t;

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(button0), gpios);

K_MSGQ_DEFINE(file, sizeof(event_t), 4, 4);
static struct gpio_callback button_cb_data;
#define DEBOUNCE_MS 75
void btn_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    static int64_t last_edge;
    int64_t now = k_uptime_get();

    bool rebond = (now - last_edge) < DEBOUNCE_MS;
    last_edge = now;
    if (rebond || gpio_pin_get_dt(&button) != 1) {
        return;
    }
    event_t evt = APPUI;
    k_msgq_put(&file, &evt, K_NO_WAIT);
}

int main(void)
{
    int ret;

    if (!gpio_is_ready_dt(&led) || !gpio_is_ready_dt(&button)) {
        printk("Erreur : GPIO LED ou bouton non prêt\n");
        return 0;
    }

    ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        printk("Erreur %d : configuration LED\n", ret);
        return 0;
    }

    ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
    if (ret < 0) {
        printk("Erreur %d : configuration bouton\n", ret);
        return 0;
    }

    ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0) {
        printk("Erreur %d : configuration interruption bouton\n", ret);
        return 0;
    }
    gpio_init_callback(&button_cb_data, btn_pressed, BIT(button.pin));
    gpio_add_callback(button.port, &button_cb_data);

    printk("Pret : chaque appui sur le bouton bascule la LED\n");

    event_t evt;
    while (1) {
        k_msgq_get(&file, &evt, K_FOREVER);
        if (evt == APPUI) {
            gpio_pin_toggle_dt(&led);
            printk("Appui -> LED basculee\n");
        }
    }
    return 0;
}
