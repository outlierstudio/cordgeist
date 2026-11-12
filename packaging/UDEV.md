Install only if USB permission diagnostics require it. Review the scoped rule,
then explicitly run these commands yourself:

    sudo install -m 644 packaging/51-cordgeist.rules /etc/udev/rules.d/51-cordgeist.rules
    sudo udevadm control --reload-rules

Unplug/reconnect the phone and unlock it. Cordgeist runs none of these commands.
The Samsung product ID is the tested normal USB mode; other normal vendor/product
IDs need an explicit local rule. This is a permissions rule, not a setup model
allowlist or a claim that an untested phone works. Active local sessions use
uaccess; headless group permissions require deliberate local administration.
