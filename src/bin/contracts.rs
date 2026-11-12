use cordgeist::{api::*, geometry::Transform};
fn main() {
    let schemas = serde_json::json!({
        "response": schemars::schema_for!(Response),
        "results": schemars::schema_for!(OperationResult),
        "request": schemars::schema_for!(Request), "leaseGrant": schemars::schema_for!(LeaseGrant),
        "actionRequest": schemars::schema_for!(ActionRequest),
        "frame": schemars::schema_for!(Frame), "receipt": schemars::schema_for!(Receipt),
        "mediaResult": schemars::schema_for!(MediaResult), "error": schemars::schema_for!(ApiError),
        "transform": schemars::schema_for!(Transform), "capability": schemars::schema_for!(Capability),
        "operations": OPERATIONS
    });
    println!("{}", serde_json::to_string_pretty(&schemas).unwrap());
}
