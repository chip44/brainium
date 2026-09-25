use color_eyre::Result;

use crate::{
    app::{APP_NAME, App},
    reader::RingSource,
    visualizer::Visualizer,
};

mod app;
mod reader;
mod ring;
mod visualizer;

fn main() -> Result {
    let source = RingSource::new()?;
    let visualizer = Visualizer::new(Box::new(source));

    Ok(eframe::run_native(
        APP_NAME,
        eframe::NativeOptions::default(),
        Box::new(|cc| Ok(Box::new(App::new(cc, visualizer)))),
    )?)
}
