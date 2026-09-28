use color_eyre::Result;

use crate::app::{APP_NAME, App};

mod app;
mod poller;
mod reader;
mod ring;
mod visualizer;

fn main() -> Result {
    Ok(eframe::run_native(
        APP_NAME,
        eframe::NativeOptions::default(),
        Box::new(|cc| Ok(Box::new(App::new(cc)))),
    )?)
}
