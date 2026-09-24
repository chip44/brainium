use color_eyre::Result;
use color_eyre::eyre::{Error, OptionExt};
use pixels::{Pixels, SurfaceTexture};
use std::sync::Arc;
use std::time::{Duration, Instant};
use winit::dpi::LogicalSize;
use winit::event_loop::ActiveEventLoop;
use winit::window::WindowId;
use winit::{
    application::ApplicationHandler,
    event::WindowEvent,
    event_loop::{ControlFlow, EventLoop},
    keyboard::{KeyCode, PhysicalKey},
    window::Window,
};

pub mod reader;
mod ring;
mod visualizer;

use crate::reader::RingSource;
use crate::visualizer::GRID_SIZE;
use visualizer::Visualizer;

const FPS: u64 = 15;
const FRAME_DURATION: Duration = Duration::from_millis(1000 / FPS);

struct App {
    window: Option<Arc<Window>>,
    pixels: Option<Pixels<'static>>,
    visualizer: Visualizer,
    last_update: Instant,
}

impl App {
    fn update(&mut self) -> Result<bool> {
        self.visualizer.process_chunk()
    }

    fn draw(&mut self) -> Result<()> {
        let pixels = self.pixels.as_mut().ok_or_eyre("pixels is uninitialized")?;
        self.visualizer.render(pixels.frame_mut());
        pixels.render()?;
        Ok(())
    }
}

impl ApplicationHandler for App {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        let window = {
            let size = LogicalSize::new(GRID_SIZE, GRID_SIZE);
            let Ok(w) = event_loop.create_window(
                Window::default_attributes()
                    .with_title("Brainium")
                    .with_inner_size(size)
                    .with_min_inner_size(size),
            ) else {
                return;
            };
            Arc::new(w)
        };

        self.window = Some(window.clone());
        self.pixels = {
            let window_size = window.inner_size();
            let surface_texture =
                SurfaceTexture::new(window_size.width, window_size.height, window.clone());
            match Pixels::new(GRID_SIZE, GRID_SIZE, surface_texture) {
                Ok(pixels) => {
                    window.request_redraw();
                    Some(pixels)
                }
                Err(e) => {
                    eprintln!("failed to create pixel buffer: {e}");
                    event_loop.exit();
                    None
                }
            }
        };
    }

    fn about_to_wait(&mut self, event_loop: &ActiveEventLoop) {
        let next_frame = self.last_update + FRAME_DURATION;

        event_loop.set_control_flow(ControlFlow::WaitUntil(next_frame));

        if Instant::now() >= next_frame
            && let Some(window) = self.window.as_ref()
        {
            window.request_redraw();
        }
    }

    fn window_event(
        &mut self,
        event_loop: &ActiveEventLoop,
        _window_id: WindowId,
        event: WindowEvent,
    ) {
        match event {
            WindowEvent::CloseRequested => {
                event_loop.exit();
            }
            WindowEvent::RedrawRequested => {
                while Instant::now() >= self.last_update + FRAME_DURATION {
                    self.last_update = Instant::now();
                    match self.update() {
                        Ok(true | false) => {}
                        Err(e) => {
                            eprintln!("error processing chunk: {e}");
                            event_loop.exit();
                            return;
                        }
                    }
                }

                if let Err(e) = self.draw() {
                    eprintln!("render error: {e}");
                }
            }
            #[allow(clippy::single_match)]
            WindowEvent::KeyboardInput { event, .. } => match event.physical_key {
                PhysicalKey::Code(KeyCode::Escape) => {
                    event_loop.exit();
                }
                _ => {}
            },
            _ => {}
        }
    }
}

fn main() -> Result<(), Error> {
    // let args: Vec<_> = std::env::args().collect();
    // if args.len() < 2 {
    //     std::process::exit(1);
    // }
    // let log_path = &args[1];
    // let source = FileSource::new(log_path).expect("failed to open log file");

    let source = RingSource::new()?;
    let visualizer = Visualizer::new(Box::new(source));

    let event_loop = EventLoop::new()?;
    event_loop.set_control_flow(ControlFlow::WaitUntil(Instant::now() + FRAME_DURATION));

    let mut app = App {
        window: None,
        pixels: None,
        visualizer,
        last_update: Instant::now(),
    };

    event_loop.run_app(&mut app)?;
    Ok(())
}
